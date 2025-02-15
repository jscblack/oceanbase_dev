/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include "share/ob_occam_time_guard.h"
#include "share/io/ob_io_struct.h"
#include "lib/ob_define.h"
#include "lib/function/ob_function.h"
#include "lib/string/ob_string.h"
#include "share/ob_occam_timer.h"
#include "ob_failure_detector.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "share/rc/ob_tenant_base.h"
#include "ob_leader_coordinator.h"
#include <utility>
#include "common_define.h"
#include "ob_leader_coordinator.h"
#include "logservice/ob_log_service.h"
#include "observer/ob_server_event_history_table_operator.h"
#include "storage/slog/ob_storage_logger.h"
#include "share/schema/ob_multi_version_schema_service.h"

namespace oceanbase
{
namespace logservice
{
namespace coordinator
{

using namespace common;

ObFailureDetector::ObFailureDetector()
    : is_running_(false),
      coordinator_(nullptr),
      has_add_clog_hang_event_(false),
      has_add_data_disk_hang_event_(false),
      has_add_clog_full_event_(false),
      has_schema_error_(false),
      has_election_silent_event_(false),
      palf_disk_hang_detector_(),
      lock_(common::ObLatchIds::ELECTION_LOCK)
{
  COORDINATOR_LOG(INFO, "ObFailureDetector constructed");
}

ObFailureDetector::~ObFailureDetector() {}

constexpr int VALUE_BUFFER_SIZE = 512;

int ObFailureDetector::mtl_init(ObFailureDetector *&p_failure_detector)
{
  int ret = OB_SUCCESS;
  ObLeaderCoordinator *coordinator = MTL(ObLeaderCoordinator *);
  if (OB_ISNULL(coordinator)) {
    ret = OB_INVALID_ARGUMENT;
    COORDINATOR_LOG(ERROR, "coordinator is nullptr", KR(ret));
  } else {
    p_failure_detector->coordinator_ = coordinator;
    COORDINATOR_LOG(INFO, "ObFailureDetector mtl init");
  }
  return ret;
}

int ObFailureDetector::mtl_start(ObFailureDetector *&p_failure_detector)
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  p_failure_detector->events_with_ops_.reset();

  if (OB_ISNULL(p_failure_detector->coordinator_)) {
    ret = OB_NOT_INIT;
    COORDINATOR_LOG(ERROR, "not init yet", KR(ret), KP_(p_failure_detector->coordinator));
  } else if (CLICK_FAIL(p_failure_detector->coordinator_->failure_detect_timer_.schedule_task_repeat(p_failure_detector->failure_task_handle_, 100_ms, [p_failure_detector]() {
    p_failure_detector->detect_failure();
    return false;
  }))) {
    COORDINATOR_LOG(ERROR, "fail to schedule failure detect task", KR(ret));
  } else if (CLICK_FAIL(p_failure_detector->coordinator_->recovery_detect_timer_.schedule_task_repeat(p_failure_detector->recovery_task_handle_, 1_s, [p_failure_detector]() {
    p_failure_detector->detect_recover();
    return false;
  }))) {
    COORDINATOR_LOG(ERROR, "fail to schedule recovery detect task", KR(ret));
  } else {
    p_failure_detector->is_running_ = true;
    COORDINATOR_LOG(INFO, "ObFailureDetector mtl start");
  }
  return ret;
}

void ObFailureDetector::mtl_stop(ObFailureDetector *&p_failure_detector)
{
  if (OB_ISNULL(p_failure_detector)) {
    COORDINATOR_LOG_RET(WARN, OB_INVALID_ARGUMENT, "p_failure_detector is NULL");
  } else {
    p_failure_detector->failure_task_handle_.stop();
    p_failure_detector->recovery_task_handle_.stop();
    COORDINATOR_LOG(INFO, "ObFailureDetector mtl init");
  }
}

void ObFailureDetector::mtl_wait(ObFailureDetector *&p_failure_detector)
{
  if (OB_ISNULL(p_failure_detector)) {
    COORDINATOR_LOG_RET(WARN, OB_INVALID_ARGUMENT, "p_failure_detector is NULL");
  } else {
    p_failure_detector->failure_task_handle_.wait();
    p_failure_detector->recovery_task_handle_.wait();
    COORDINATOR_LOG(INFO, "ObFailureDetector mtl init");
  }
}

void ObFailureDetector::destroy()
{
  LC_TIME_GUARD(1_s);
  has_add_clog_hang_event_ = false;
  has_add_data_disk_hang_event_ = false;
  has_add_clog_full_event_ = false;
  has_schema_error_ = false;
  has_election_silent_event_ = false;
  COORDINATOR_LOG(INFO, "ObFailureDetector mtl destroy");
}

void ObFailureDetector::detect_recover()
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  ObArray<FailureEventWithRecoverOp> temp_events_with_ops;
  ObSpinLockGuard guard(lock_);
  if (!events_with_ops_.empty()) {
    COORDINATOR_LOG(INFO, "doing detect recover operation", K_(events_with_ops));
  }
  for (int64_t idx = 0; idx < events_with_ops_.count() && OB_SUCC(ret); ++idx) {
    if (events_with_ops_[idx].recover_detect_operation_.is_valid()) {
      LC_TIME_GUARD(10_ms);
      bool recover_flag = events_with_ops_[idx].recover_detect_operation_();
      if (recover_flag) {
        COORDINATOR_LOG(INFO, "revocer event detected", K(events_with_ops_[idx]));
        if (CLICK_FAIL(events_with_ops_.remove(idx))) {
          COORDINATOR_LOG(WARN, "fail to remove", K(events_with_ops_[idx]));
        } else {
          (void) insert_event_to_table_(events_with_ops_[idx].event_, events_with_ops_[idx].recover_detect_operation_, "DETECT REVOCER");
          --idx;// next loop is still access to idx
        }
      }
    }
  }
}

void ObFailureDetector::detect_failure()
{
  LC_TIME_GUARD(1_s);
  // clog disk hang check
  detect_palf_hang_failure_();
  // data disk io hang check
  detect_data_disk_io_failure_();
  // clog disk full check
  detect_palf_disk_full_();
  // schema refreshed check
  detect_schema_not_refreshed_();
#ifdef OB_BUILD_ARBITRATION
  // election silent check
  detect_election_silent_();
#endif
}

int ObFailureDetector::add_failure_event(const FailureEvent &event)
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  int64_t idx = 0;
  FailureEventWithRecoverOp event_with_op;
  ObSpinLockGuard guard(lock_);
  if (!check_is_running_()) {
    ret = OB_NOT_RUNNING;
    COORDINATOR_LOG(WARN, "not running", KR(ret), K(event), K(events_with_ops_));
  } else {
    for (; idx < events_with_ops_.count(); ++idx) {
      if (events_with_ops_[idx].event_ == event) {
        break;
      }
    }
    if (idx != events_with_ops_.count()) {
      ret = OB_ENTRY_EXIST;
      COORDINATOR_LOG(WARN, "this failure event has been exist", KR(ret), K(event), K(events_with_ops_));
    } else if (CLICK_FAIL(event_with_op.init(event, ObFunction<bool()>()))) {
      COORDINATOR_LOG(WARN, "fail to init event with op", KR(ret), K(event), K(events_with_ops_));
    } else if (CLICK_FAIL(events_with_ops_.push_back(event_with_op))) {
      COORDINATOR_LOG(WARN, "fail to push", KR(ret), K(event), K(events_with_ops_));
    } else {
      COORDINATOR_LOG(INFO, "success report a failure event without recover detect operation", KR(ret), K(event), K(events_with_ops_));
      (void) insert_event_to_table_(event, ObFunction<bool()>(), event.info_.get_ob_string());
    }
  }
  return ret;
}

int ObFailureDetector::add_failure_event(const FailureEvent &event, const ObFunction<bool()> &recover_detect_operation)
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  int64_t idx = 0;
  FailureEventWithRecoverOp event_with_op;
  ObSpinLockGuard guard(lock_);
  if (!check_is_running_()) {
    ret = OB_NOT_RUNNING;
    COORDINATOR_LOG(WARN, "not running", KR(ret), K(event), K(events_with_ops_));
  } else {
    for (; idx < events_with_ops_.count(); ++idx) {
      if (events_with_ops_[idx].event_ == event) {
        break;
      }
    }
    if (idx != events_with_ops_.count()) {
      ret = OB_ENTRY_EXIST;
      COORDINATOR_LOG(WARN, "this failure event has been exist", KR(ret), K(event), K(events_with_ops_));
    } else if (CLICK_FAIL(event_with_op.init(event, recover_detect_operation))) {
      COORDINATOR_LOG(WARN, "fail to init event with op", KR(ret), K(event), K(events_with_ops_));
    } else if (CLICK_FAIL(events_with_ops_.push_back(event_with_op))) {
      COORDINATOR_LOG(WARN, "fail to push", KR(ret), K(event), K(events_with_ops_));
    } else {
      COORDINATOR_LOG(INFO, "success report a failure event with recover detect operation", KR(ret), K(event), K(events_with_ops_));
      (void) insert_event_to_table_(event, recover_detect_operation, event.info_.get_ob_string());
    }
  }
  return ret;
}

int ObFailureDetector::remove_failure_event(const FailureEvent &event)
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  int64_t idx = 0;
  ObSpinLockGuard guard(lock_);
  if (!check_is_running_()) {
    ret = OB_NOT_RUNNING;
    COORDINATOR_LOG(WARN, "not running", KR(ret), K(event), K(events_with_ops_));
  } else {
    for (; idx < events_with_ops_.count(); ++idx) {
      if (events_with_ops_[idx].event_ == event) {
        break;
      }
    }
    if (idx == events_with_ops_.count()) {
      ret = OB_ENTRY_NOT_EXIST;
      COORDINATOR_LOG(WARN, "this failure event not exist", KR(ret), K(event), K(events_with_ops_));
    } else {
      (void) insert_event_to_table_(events_with_ops_[idx].event_, events_with_ops_[idx].recover_detect_operation_, "REMOVE FAILURE");
      if (CLICK_FAIL(events_with_ops_.remove(idx))) {
        COORDINATOR_LOG(WARN, "remove event failed", KR(ret), K(event), K(events_with_ops_));
      } else {
        COORDINATOR_LOG(INFO, "user remove failure event success", KR(ret), K(event), K(events_with_ops_));
      }
    }
  }
  return ret;
}

int ObFailureDetector::get_specified_level_event(FailureLevel level, ObIArray<FailureEvent> &results)
{
  LC_TIME_GUARD(1_s);
  #define PRINT_WRAPPER KR(ret), K(level), K(results)
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);
  if (!check_is_running_()) {
    ret = OB_NOT_RUNNING;
    COORDINATOR_LOG(WARN, "not running", KR(ret), K(events_with_ops_));
  } else {
    results.reset();
    for (int64_t idx = 0; idx < events_with_ops_.count(); ++idx) {
      if (events_with_ops_.at(idx).event_.get_failure_level() == level) {
        if (CLICK_FAIL(results.push_back(events_with_ops_.at(idx).event_))) {
          COORDINATOR_LOG_(WARN, "fail to push back event to results");
        }
      }
    }
    if (CLICK_FAIL(ret)) {
      COORDINATOR_LOG_(WARN, "fail to get specified level failure event");
    }
  }
  return ret;
  #undef PRINT_WRAPPER
}

int ObFailureDetector::insert_event_to_table_(const FailureEvent &event, const ObFunction<bool()> &recover_operation, ObString info)
{
  LC_TIME_GUARD(1_s);
  #define PRINT_WRAPPER KR(ret), K(event), K(recover_operation)
  int ret = OB_SUCCESS;
  if (CLICK_FAIL(SERVER_EVENT_ADD("FAILURE_DETECTOR",
                                  common::to_cstring(info),
                                  "FAILURE_MODULE",
                                  obj_to_cstring(event.module_),
                                  "FAILURE_TYPE",
                                  obj_to_cstring(event.type_),
                                  "AUTO_RECOVER",
                                  common::to_cstring(recover_operation.is_valid())))) {
    COORDINATOR_LOG_(WARN, "insert into __all_server_event_history failed");
  } else {
    COORDINATOR_LOG_(INFO, "insert into __all_server_event_history success");
  }
  return ret;
  #undef PRINT_WRAPPER
}

bool ObFailureDetector::is_clog_disk_has_fatal_error()
{
  return ATOMIC_LOAD(&has_add_clog_hang_event_)
         || ATOMIC_LOAD(&has_add_clog_full_event_);
}

bool ObFailureDetector::is_data_disk_has_fatal_error()
{
  return ATOMIC_LOAD(&has_add_data_disk_hang_event_);
}

bool ObFailureDetector::is_schema_not_refreshed()
{
  return ATOMIC_LOAD(&has_schema_error_);
}

void ObFailureDetector::detect_palf_hang_failure_()
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  const int64_t BUF_LEN = 30;
  char infos[BUF_LEN];
  int64_t sensitivity = 0;
  const int64_t now = ObTimeUtility::current_time();

  const bool has_failure = ATOMIC_LOAD(&has_add_clog_hang_event_);
  const bool is_clog_disk_hang = palf_disk_hang_detector_.is_clog_disk_hang(sensitivity);
  FailureEvent clog_disk_hang_event(FailureType::PROCESS_HANG, FailureModule::LOG, FailureLevel::FATAL);
  ::oceanbase::common::databuff_printf(infos, BUF_LEN, "clog disk hang, sen: %ld", sensitivity);

  if (OB_FAIL(clog_disk_hang_event.set_info(infos))) {
    COORDINATOR_LOG(ERROR, "clog_disk_hang_event set_info failed", K(ret));
  } else if (false == has_failure) {
    if (!is_clog_disk_hang) {
      // log disk does not hang, skip.
    } else if (OB_FAIL(add_failure_event(clog_disk_hang_event))) {
      COORDINATOR_LOG(ERROR, "add_failure_event failed", K(ret), K(clog_disk_hang_event));
    } else {
      ATOMIC_SET(&has_add_clog_hang_event_, true);
      LOG_DBA_ERROR(OB_DISK_HUNG, "msg", "clog disk may be hung, add failure event", K(clog_disk_hang_event));
    }
  } else {
    if (is_clog_disk_hang) {
      // IO worker has not recoverd, cannot remove failure_event.
    } else if (OB_FAIL(remove_failure_event(clog_disk_hang_event))) {
      COORDINATOR_LOG(ERROR, "remove_failure_event failed", K(ret), K(clog_disk_hang_event));
    } else {
      ATOMIC_SET(&has_add_clog_hang_event_, false);
      COORDINATOR_LOG(INFO, "clog disk has recoverd, remove failure event", K(ret), K(clog_disk_hang_event));
    }
  }
}

void ObFailureDetector::detect_data_disk_io_failure_()
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  ObDeviceHealthStatus data_disk_status;
  int64_t data_disk_error_start_ts = OB_INVALID_TIMESTAMP;
  const int64_t now = ObTimeUtility::current_time();
  FailureEvent data_disk_io_hang_event(FailureType::PROCESS_HANG, FailureModule::STORAGE, FailureLevel::FATAL);
  if (OB_FAIL(data_disk_io_hang_event.set_info("data disk io hang event"))) {
    COORDINATOR_LOG(ERROR, "sstable_io_hang_event set_info failed", K(ret));
  } else if (OB_FAIL(OB_IO_MANAGER.get_device_health_detector().get_device_health_status(data_disk_status,
                                                                                         data_disk_error_start_ts))) {
    COORDINATOR_LOG(WARN, "get_device_health_status failed", K(ret));
  } else if (false == ATOMIC_LOAD(&has_add_data_disk_hang_event_)) {
    // TODO: modify statement if new ObDeviceHealthStatus is added
    if (ObDeviceHealthStatus::DEVICE_HEALTH_NORMAL == data_disk_status) {
      // data disk does not hang, skip.
    } else if (OB_FAIL(add_failure_event(data_disk_io_hang_event))) {
      COORDINATOR_LOG(ERROR, "add_failure_event failed", K(ret), K(data_disk_io_hang_event));
    } else {
      ATOMIC_SET(&has_add_data_disk_hang_event_, true);
      LOG_DBA_ERROR(OB_DISK_HUNG, "msg", "data disk may be hung, add failure event", K(data_disk_io_hang_event),
                    K(data_disk_error_start_ts));
    }
  } else {
    if (ObDeviceHealthStatus::DEVICE_HEALTH_NORMAL != data_disk_status) {
      // data disk does not recoverd, cannot remove failure_event.
    } else if (OB_FAIL(remove_failure_event(data_disk_io_hang_event))) {
      COORDINATOR_LOG(ERROR, "remove_failure_event failed", K(ret), K(data_disk_io_hang_event));
    } else {
      ATOMIC_SET(&has_add_data_disk_hang_event_, false);
      COORDINATOR_LOG(INFO, "data disk has recoverd, remove failure event", K(ret), K(data_disk_io_hang_event));
    }
  }
}

void ObFailureDetector::detect_palf_disk_full_()
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  bool is_disk_enough = true;
  ObLogService *log_service = MTL(ObLogService*);
  FailureEvent clog_disk_full_event(FailureType::RESOURCE_NOT_ENOUGH, FailureModule::LOG, FailureLevel::FATAL);
  if (OB_FAIL(clog_disk_full_event.set_info("clog disk full event"))) {
    COORDINATOR_LOG(ERROR, "clog_disk_full_event set_info failed", K(ret));
  } else if (OB_FAIL(log_service->check_disk_space_enough(is_disk_enough))) {
    COORDINATOR_LOG(WARN, "check_disk_space_enough failed", K(ret));
  } else if (false == ATOMIC_LOAD(&has_add_clog_full_event_)) {
    if (is_disk_enough) {
      // clog disk is not full, skip.
    } else if (OB_FAIL(add_failure_event(clog_disk_full_event))) {
      COORDINATOR_LOG(ERROR, "add_failure_event failed", K(ret), K(clog_disk_full_event));
    } else {
      ATOMIC_SET(&has_add_clog_full_event_, true);
      LOG_DBA_ERROR(OB_LOG_OUTOF_DISK_SPACE, "msg", "clog disk is almost full, add failure event",
                    K(clog_disk_full_event), K(now));
    }
  } else {
    if (!is_disk_enough) {
      // clog disk is still full, cannot remove failure_event.
    } else if (OB_FAIL(remove_failure_event(clog_disk_full_event))) {
      COORDINATOR_LOG(ERROR, "remove_failure_event failed", K(ret), K(clog_disk_full_event));
    } else {
      ATOMIC_SET(&has_add_clog_full_event_, false);
      COORDINATOR_LOG(INFO, "clog disk has left space, remove failure event", K(ret), K(clog_disk_full_event));
    }
  }
}

void ObFailureDetector::detect_schema_not_refreshed_()
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  bool schema_not_refreshed = GSCHEMASERVICE.is_tenant_not_refreshed(MTL_ID());
  FailureEvent schema_not_refreshed_event(FailureType::SCHEMA_NOT_REFRESHED, FailureModule::SCHEMA, FailureLevel::SERIOUS);
  if (OB_FAIL(schema_not_refreshed_event.set_info("schema not refreshed"))) {
    COORDINATOR_LOG(ERROR, "schema_not_refreshed_event  set_info failed", KR(ret));
  } else if (false == ATOMIC_LOAD(&has_schema_error_)) {
    if (!schema_not_refreshed) {
      // schema has been refreshed, skip.
    } else if (OB_FAIL(add_failure_event(schema_not_refreshed_event))) {
      COORDINATOR_LOG(ERROR, "add_failure_event failed", KR(ret), K(schema_not_refreshed));
    } else {
      ATOMIC_SET(&has_schema_error_, true);
      COORDINATOR_LOG(WARN, "schema not refreshed, add failure event",
                    K(schema_not_refreshed), K(now));
    }
  } else {
    if (schema_not_refreshed) {
      // schema is still not refreshed, cannot remove failure_event.
    } else if (OB_FAIL(remove_failure_event(schema_not_refreshed_event))) {
      COORDINATOR_LOG(ERROR, "remove_failure_event failed", KR(ret), K(schema_not_refreshed));
    } else {
      ATOMIC_SET(&has_schema_error_, false);
      COORDINATOR_LOG(INFO, "schema is refreshed, remove failure event", KR(ret), K(schema_not_refreshed));
    }
  }
}
#ifdef OB_BUILD_ARBITRATION
void ObFailureDetector::detect_election_silent_()
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;

  logservice::ObLogService *log_service = MTL(logservice::ObLogService*);
  if (OB_ISNULL(log_service)) {
    ret = OB_ERR_UNEXPECTED;
    COORDINATOR_LOG(ERROR, "ptr is null, unexpected error", K(ret));
  } else {
    bool is_election_silent = false;
    FailureEvent election_silent_event(FailureType::ENTER_ELECTION_SILENT, FailureModule::LOG, FailureLevel::FATAL);
    GetElectionSilentFunctor functor(is_election_silent);
    PalfEnv *palf_env = log_service->get_palf_env();
    if (OB_ISNULL(palf_env)) {
      ret = OB_ERR_UNEXPECTED;
      COORDINATOR_LOG(ERROR, "palf_env is null, unexpected error", K(ret));
    } else if (OB_FAIL(palf_env->for_each(functor))){
      COORDINATOR_LOG(WARN, "GetElectionSilentFunctor failed", K(ret));
    } else {
    }

    if (OB_FAIL(ret)) {
    } else if (false == ATOMIC_LOAD(&has_election_silent_event_)) {
      if (false == is_election_silent) {
        // no need to add failure event
      } else if (OB_FAIL(add_failure_event(election_silent_event))) {
        COORDINATOR_LOG(ERROR, "add_failure_event failed", K(ret), K(election_silent_event));
      } else {
        ATOMIC_SET(&has_election_silent_event_, true);
        COORDINATOR_LOG(INFO, "add election silent failure event", K(ret), K(election_silent_event));
      }
    } else {
      if (true == is_election_silent) {
        // still in silent state, can't remove
      } else if (OB_FAIL(remove_failure_event(election_silent_event))) {
        COORDINATOR_LOG(ERROR, "remove_failure_event failed", K(ret), K(election_silent_event));
      } else {
        ATOMIC_SET(&has_election_silent_event_, false);
        COORDINATOR_LOG(INFO, "remove election silent failure event", K(ret), K(election_silent_event));
      }
    }
  }
}
#endif

int ObFailureDetector::FailureEventWithRecoverOp::init(const FailureEvent &event,
                                                       const ObFunction<bool()> &recover_detect_operation)
{
  LC_TIME_GUARD(1_s);
  int ret = OB_SUCCESS;
  if (CLICK_FAIL(event_.assign(event))) {
    COORDINATOR_LOG(WARN, "fail to assign event", K(ret));
  } else if (CLICK_FAIL(recover_detect_operation_.assign(recover_detect_operation))) {
    COORDINATOR_LOG(WARN, "fail to assign op", K(ret));
  }
  return ret;
}

int ObFailureDetector::FailureEventWithRecoverOp::assign(const FailureEventWithRecoverOp &rhs)
{
  return init(rhs.event_, rhs.recover_detect_operation_);
}

ObFailureDetector::PalfDiskHangDetector::PalfDiskHangDetector() :
    last_detect_time_(OB_INVALID_TIMESTAMP),
    last_detect_failure_time_(OB_INVALID_TIMESTAMP),
    curr_detect_round_(0),
    prev_accum_write_size_(0),
    prev_accum_write_count_(0),
    prev_accum_write_rt_(0)
{
  for (int i = 0; i < PALF_DISK_LEARN_SLOT; i++) {
    learn_avg_bw_[i] = -1;
    learn_avg_rt_[i] = -1;
  }
  for (int i = 0; i < MIN_RECOVERY_INTERVAL; i++) {
    detect_error_flags_[i] = false;
  }
}

ObFailureDetector::PalfDiskHangDetector::~PalfDiskHangDetector()
{
  last_detect_time_ = OB_INVALID_TIMESTAMP;
  last_detect_failure_time_ = OB_INVALID_TIMESTAMP;
  curr_detect_round_ = 0;
  prev_accum_write_size_ = 0;
  prev_accum_write_count_ = 0;
  prev_accum_write_rt_ = 0;
}

int64_t ObFailureDetector::PalfDiskHangDetector::min_recovery_interval_() const
{
  return MIN_RECOVERY_INTERVAL;
}

int64_t ObFailureDetector::PalfDiskHangDetector::size_to_learn_idx_(const double log_size) const
{
  int64_t slot_idx = 0;
  if (log_size <= MIN_WRITE_SIZE) {
    slot_idx = 0;
  } else {
    const int exp = static_cast<int>(std::log10(log_size));
    const double base = log_size / std::pow(10, exp);
    if (base > 4) {
      slot_idx = ((base - 4) / 0.1) + (90 * (exp - 3));
    } else {
      slot_idx = (90 * (exp - 3)) - ((4 - base) / 0.1);
    }
    if (slot_idx >= PALF_DISK_LEARN_SLOT) {
      slot_idx = PALF_DISK_LEARN_SLOT - 1;
    }
  }
  return slot_idx;
}

double ObFailureDetector::PalfDiskHangDetector::learn_idx_to_size_(const int64_t learn_idx) const
{
  double size = 0.0;
  if (learn_idx >= 0 && learn_idx < PALF_DISK_LEARN_SLOT) {
    int64_t exp = 3;
    double base = 4 + learn_idx * 0.1;
    while (base > 9) {
      base -= 9;
      exp++;
    }
    size = base * std::pow(10, exp);
  }
  return size;
}

bool ObFailureDetector::PalfDiskHangDetector::has_continuous_error_(
    const bool has_failure,
    const int64_t continuous_error_gap) const
{
  bool bool_ret = false;
  int64_t error_count = 0;
  for (int i = curr_detect_round_; i >= 0 && i > curr_detect_round_ - continuous_error_gap; i--) {
    error_count += (detect_error_flags_[i % MIN_RECOVERY_INTERVAL]? 1: 0);
  }

  if (has_failure) {
    bool_ret = (error_count != 0);
  } else {
    bool_ret = (error_count > continuous_error_gap / 2);
  }
  return bool_ret;
}

bool ObFailureDetector::PalfDiskHangDetector::is_clog_disk_hang(int64_t &sensitivity)
{
  int ret = OB_SUCCESS;
  bool bool_ret = false;
  const bool has_failure = (OB_INVALID_TIMESTAMP != last_detect_failure_time_);
  ObLogService *log_service = MTL(ObLogService*);
  int64_t last_working_time = OB_INVALID_TIMESTAMP;
  int64_t pending_write_size = 0;
  int64_t pending_write_count = 0;
  int64_t pending_write_rt = 0;
  int64_t accum_write_size = 0;
  int64_t accum_write_count = 0;
  int64_t accum_write_rt = 0;
  const int64_t now = ObTimeUtility::current_time();
  const int64_t tolerance_time = GCONF.log_storage_warning_tolerance_time;
  sensitivity = GCONF.log_storage_warning_trigger_percentage;
  if (OB_ISNULL(log_service)) {
    ret = OB_ERR_UNEXPECTED;
    COORDINATOR_LOG(ERROR, "log_service is NULL", K(ret));
  } else if (OB_INVALID_TIMESTAMP != last_detect_time_ &&
      now - last_detect_time_ < PALF_DISK_DETECT_INTERVAL_US) {
    ret = OB_EAGAIN;
  } else if (OB_FAIL(log_service->get_io_statistic_info(last_working_time,
      pending_write_size, pending_write_count, pending_write_rt,
      accum_write_size, accum_write_count, accum_write_rt))) {
    COORDINATOR_LOG(WARN, "get_io_statistic_info failed", K(ret));
  } else {
    last_detect_time_ = now;
    const double bw_warn_ratio = 0.5;
    const double bw_error_ratio = MIN(0.5, 0.01 * sensitivity);
    const int64_t continuous_error_gap = (has_failure)? min_recovery_interval_(): \
        tolerance_time / PALF_DISK_DETECT_INTERVAL_US;

    // record statistics every PALF_DISK_DETECT_INTERVAL_US, regardless of whether a failure exists
    const int64_t this_write_size = accum_write_size - prev_accum_write_size_;
    const int64_t this_write_count = accum_write_count - prev_accum_write_count_;
    const int64_t this_write_rt = accum_write_rt - prev_accum_write_rt_;
    prev_accum_write_size_ = accum_write_size;
    prev_accum_write_count_ = accum_write_count;
    prev_accum_write_rt_ = accum_write_rt;
    // IO perf of last PALF_DISK_DETECT_INTERVAL_US
    const double this_avg_bw = (this_write_rt <= 0)? 0: ((double)(this_write_size) * 1000 * 1000 / this_write_rt);
    const double this_avg_size = (this_write_count <= 0)? 0: ((double)(this_write_size) / this_write_count);
    const double this_avg_rt = (this_write_count <= 0)? 0: ((double)(this_write_rt) / this_write_count);
    // pending io perf
    const double pending_avg_bw = (pending_write_rt <= 0)? 0: ((double)(pending_write_size) * 1000 * 1000 / pending_write_rt);
    const double pending_avg_size = (pending_write_count <= 0)? 0: ((double)(pending_write_size) / pending_write_count);
    const double pending_avg_rt = (pending_write_count <= 0)? 0: ((double)(pending_write_rt) / pending_write_count);

    double warn_baseline_bw, warn_baseline_rt, warn_baseline_size = 1;
    double error_baseline_bw, error_baseline_rt, error_baseline_size = 1;

    // recognize bandwidth failure based on learned perf
    // to find: baseline_size < this_size and baseline_rt < this_rt and baseline_bw * ratio > this_bw
    bool is_perf_decrease_warn = false;
    bool is_perf_decrease_error = false;
    for (int64_t i = size_to_learn_idx_(this_avg_size) - 1; this_write_count > 0 && i >= 0; i--) {
      if (false == is_perf_decrease_warn &&
          learn_avg_rt_[i] > 0 && learn_avg_rt_[i] < this_avg_rt &&
          learn_avg_bw_[i] > 0 && learn_avg_bw_[i] * bw_warn_ratio > this_avg_bw) {
        is_perf_decrease_warn = true;
        warn_baseline_bw = learn_avg_bw_[i];
        warn_baseline_rt = learn_avg_rt_[i];
        warn_baseline_size = learn_idx_to_size_(i);
      }
      if (false == is_perf_decrease_error &&
          learn_avg_rt_[i] > 0 && learn_avg_rt_[i] < this_avg_rt &&
          learn_avg_bw_[i] > 0 && learn_avg_bw_[i] * bw_error_ratio > this_avg_bw) {
        is_perf_decrease_error = true;
        error_baseline_bw = learn_avg_bw_[i];
        error_baseline_rt = learn_avg_rt_[i];
        error_baseline_size = learn_idx_to_size_(i);
      }
      if (true == is_perf_decrease_error) {
        break;
      }
    }
    // recognize pending IO failure based on learned perf
    const bool has_long_pending_io = (OB_INVALID_TIMESTAMP != last_working_time
        && now - last_working_time > tolerance_time);
    const bool check_small_pending_io = (pending_avg_rt > PALF_DISK_DETECT_INTERVAL_US &&
        false == has_long_pending_io && false == is_perf_decrease_error);
    bool has_small_pending_io = false;
    for (int64_t i = size_to_learn_idx_(pending_avg_size) - 1; true == check_small_pending_io && i >= 0; i--) {
      if (learn_avg_bw_[i] > 0 && learn_avg_bw_[i] * bw_error_ratio > this_avg_bw + pending_avg_bw) {
        has_small_pending_io = true;
        error_baseline_bw = learn_avg_bw_[i];
        error_baseline_rt = learn_avg_rt_[i];
        error_baseline_size = learn_idx_to_size_(i);
        break;
      }
    }

    detect_error_flags_[curr_detect_round_ % MIN_RECOVERY_INTERVAL] = \
        (is_perf_decrease_error || has_small_pending_io || has_long_pending_io);
    bool has_continuous_error = has_continuous_error_(has_failure, continuous_error_gap);

    // learn perf data unless there are warning or error
    if (false == is_perf_decrease_warn &&
        false == is_perf_decrease_error &&
        false == has_small_pending_io &&
        false == has_long_pending_io &&
        false == has_failure) {
      const int64_t this_idx = size_to_learn_idx_(this_avg_size);
      if (learn_avg_bw_[this_idx] <= 0 || learn_avg_rt_[this_idx] <= 0) {
        learn_avg_bw_[this_idx] = this_avg_bw;
        learn_avg_rt_[this_idx] = this_avg_rt;
      } else {
        learn_avg_bw_[this_idx] = (this_avg_bw + 9 * learn_avg_bw_[this_idx]) / 10;
        learn_avg_rt_[this_idx] = (this_avg_rt + 9 * learn_avg_rt_[this_idx]) / 10;
      }
    }

    if (false == has_failure) {
      if (((has_small_pending_io || is_perf_decrease_error) && has_continuous_error) ||
          has_long_pending_io) {
        bool_ret = true;
        last_detect_failure_time_ = now;
      } else {
        bool_ret = false;
      }
    } else {
      // failure recovery
      if (false == has_small_pending_io &&
          false == has_long_pending_io &&
          (0 == sensitivity ||
          (false == is_perf_decrease_error && false == has_continuous_error) ||
          now - last_detect_failure_time_ > PALF_DISK_FAILURE_TIME_UPPER_BOUND)) {
        bool_ret = false;
      } else {
        bool_ret = true;
      }
    }
    if (has_failure != bool_ret || is_perf_decrease_warn ||
        is_perf_decrease_error || has_small_pending_io || has_long_pending_io ||
        REACH_TENANT_TIME_INTERVAL(30 * 1000 * 1000)) {
      COORDINATOR_LOG(INFO, "is_clog_disk_hang finish", K(bool_ret), K(has_failure),
          K(is_perf_decrease_warn), K(is_perf_decrease_error), K(has_continuous_error),
          K(has_small_pending_io), K(has_long_pending_io),
          K(last_working_time), K(sensitivity), K(tolerance_time), KPC(this),
          K(this_write_count), K(pending_write_count),
          K(this_avg_bw), K(warn_baseline_bw), K(error_baseline_bw), K(pending_avg_bw),
          K(this_avg_size), K(error_baseline_size),
          K(this_avg_rt), K(warn_baseline_rt), K(error_baseline_rt));
    }
    curr_detect_round_++;
  }

  if (OB_FAIL(ret)) {
    bool_ret = has_failure;
  } else if (false == bool_ret) {
    last_detect_failure_time_ = OB_INVALID_TIMESTAMP;
  } else { }
  return bool_ret;
}
}
}
}
