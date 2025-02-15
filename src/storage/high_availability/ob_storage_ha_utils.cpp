// Copyright (c) 2022 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.

#define USING_LOG_PREFIX STORAGE

#include "ob_storage_ha_utils.h"
#include "observer/ob_server_struct.h"
#include "share/config/ob_server_config.h"
#include "share/location_cache/ob_location_service.h"
#include "share/ob_zone_merge_info.h"
#include "storage/tablet/ob_tablet.h"
#include "share/tablet/ob_tablet_table_operator.h"
#include "share/ob_global_merge_table_operator.h"
#include "share/ob_tablet_replica_checksum_operator.h"
#include "share/scn.h"
#include "share/ls/ob_ls_info.h"
#include "ob_storage_ha_struct.h"
#include "share/ls/ob_ls_table_operator.h"
#include "observer/ob_server_event_history_table_operator.h"
#include "observer/ob_server_struct.h"
#include "observer/ob_service.h"
#include "share/ob_version.h"
#include "share/ob_cluster_version.h"
#include "storage/ob_storage_rpc.h"
#include "storage/tx/ob_ts_mgr.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "rootserver/ob_tenant_info_loader.h"
#include "src/observer/omt/ob_tenant_config.h"
#include "common/errsim_module/ob_errsim_module_type.h"
#include "common/ob_role.h"

using namespace oceanbase::share;

namespace oceanbase
{
namespace storage
{
ERRSIM_POINT_DEF(EN_CHECK_LOG_NEED_REBUILD);
int ObStorageHAUtils::get_ls_leader(const uint64_t tenant_id, const share::ObLSID &ls_id, common::ObAddr &leader)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  static const int64_t DEFAULT_CHECK_LS_LEADER_TIMEOUT = 1 * 60 * 1000 * 1000L;  // 1min
  const int64_t cluster_id = GCONF.cluster_id;
  if (OB_ISNULL(GCTX.location_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("location cache is NULL", K(ret));
  } else if (OB_INVALID_ID == tenant_id || !ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid args", K(ret), K(tenant_id), K(ls_id));
  } else {
    uint32_t renew_count = 0;
    const uint32_t max_renew_count = 10;
    const int64_t retry_us = 200 * 1000;
    const int64_t start_ts = ObTimeUtility::current_time();
    do {
      if (OB_FAIL(GCTX.location_service_->nonblock_get_leader(cluster_id, tenant_id, ls_id, leader))) {
        if (OB_LS_LOCATION_NOT_EXIST == ret && renew_count++ < max_renew_count) {  // retry ten times
          LOG_WARN("failed to get location and force renew", K(ret), K(tenant_id), K(ls_id), K(cluster_id));
          if (OB_SUCCESS != (tmp_ret = GCTX.location_service_->nonblock_renew(cluster_id, tenant_id, ls_id))) {
            LOG_WARN("failed to nonblock renew from location cache", K(tmp_ret), K(ls_id), K(cluster_id));
          } else if (ObTimeUtility::current_time() - start_ts > DEFAULT_CHECK_LS_LEADER_TIMEOUT) {
            renew_count = max_renew_count;
          } else {
            ob_usleep(retry_us);
          }
        }
      } else {
        LOG_INFO("get ls leader", K(tenant_id), K(ls_id), K(leader), K(cluster_id));
      }
    } while (OB_LS_LOCATION_NOT_EXIST == ret && renew_count < max_renew_count);

    if (OB_SUCC(ret) && !leader.is_valid()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("leader addr is invalid", K(ret), K(tenant_id), K(ls_id), K(leader), K(cluster_id));
    }
  }
  return ret;
}

int ObStorageHAUtils::check_tablet_replica_validity(const uint64_t tenant_id, const share::ObLSID &ls_id,
    const common::ObAddr &src_addr, const common::ObTabletID &tablet_id, common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  SCN compaction_scn;
  if (tablet_id.is_ls_inner_tablet()) {
    // do nothing
  } else if (OB_INVALID_ID == tenant_id || !ls_id.is_valid() || !src_addr.is_valid() || !tablet_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid args", K(ret), K(tenant_id), K(ls_id), K(src_addr), K(tablet_id));
  } else if (OB_FAIL(check_merge_error_(tenant_id, sql_client))) {
    LOG_WARN("failed to check merge error", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(fetch_src_tablet_meta_info_(tenant_id, tablet_id, ls_id, src_addr, sql_client, compaction_scn))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      LOG_INFO("tablet may not has major sstable, no need check", K(tenant_id), K(tablet_id), K(ls_id), K(src_addr));
    } else {
      LOG_WARN("failed to fetch src tablet meta info", K(ret), K(tenant_id), K(tablet_id), K(ls_id), K(src_addr));
    }
  } else if (OB_FAIL(check_tablet_replica_checksum_(tenant_id, tablet_id, ls_id, compaction_scn, sql_client))) {
    LOG_WARN("failed to check tablet replica checksum", K(ret), K(tenant_id), K(tablet_id), K(ls_id), K(compaction_scn));
  }
  return ret;
}

int ObStorageHAUtils::get_server_version(uint64_t &server_version)
{
  int ret = OB_SUCCESS;
  server_version = CLUSTER_CURRENT_VERSION;
  return ret;
}

int ObStorageHAUtils::check_server_version(const uint64_t server_version)
{
  int ret = OB_SUCCESS;
  uint64_t cur_server_version = 0;
  if (OB_FAIL(get_server_version(cur_server_version))) {
    LOG_WARN("failed to get server version", K(ret));
  } else {
    bool can_migrate = cur_server_version >= server_version;
    if (!can_migrate) {
      ret = OB_MIGRATE_NOT_COMPATIBLE;
      LOG_WARN("migrate server not compatible", K(ret), K(server_version), K(cur_server_version));
    }
  }
  return ret;
}

int ObStorageHAUtils::report_ls_meta_table(const uint64_t tenant_id, const share::ObLSID &ls_id,
    const storage::ObMigrationStatus &migration_status)
{
  int ret = OB_SUCCESS;
  share::ObLSReplica ls_replica;
  share::ObLSTableOperator *lst_operator = GCTX.lst_operator_;
  const bool inner_table_only = false;
  if (OB_FAIL(GCTX.ob_service_->fill_ls_replica(tenant_id, ls_id, ls_replica))) {
    LOG_WARN("failed to fill ls replica", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(lst_operator->update(ls_replica, inner_table_only))) {
    LOG_WARN("failed to update ls meta table", K(ret), K(ls_replica));
  } else {
    SERVER_EVENT_ADD("storage_ha", "report_ls_meta_table",
                      "tenant_id", tenant_id,
                      "ls_id", ls_id,
                      "migration_status", migration_status);
    LOG_INFO("report ls meta table", K(ls_replica));
  }
  return ret;
}

int ObStorageHAUtils::check_merge_error_(const uint64_t tenant_id, common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  share::ObGlobalMergeInfo merge_info;
  if (OB_FAIL(ObGlobalMergeTableOperator::load_global_merge_info(sql_client, tenant_id, merge_info))) {
    LOG_WARN("failed to laod global merge info", K(ret), K(tenant_id));
  } else if (merge_info.is_merge_error()) {
    ret = OB_CHECKSUM_ERROR;
    LOG_ERROR("merge error, can not migrate", K(ret), K(tenant_id), K(merge_info));
  }
  return ret;
}

int ObStorageHAUtils::fetch_src_tablet_meta_info_(const uint64_t tenant_id, const common::ObTabletID &tablet_id,
    const share::ObLSID &ls_id, const common::ObAddr &src_addr, common::ObISQLClient &sql_client, SCN &compaction_scn)
{
  int ret = OB_SUCCESS;
  ObTabletTableOperator op;
  ObTabletReplica tablet_replica;
  if (OB_FAIL(op.init(sql_client))) {
    LOG_WARN("failed to init operator", K(ret));
  } else if (OB_FAIL(op.get(tenant_id, tablet_id, ls_id, src_addr, tablet_replica))) {
    LOG_WARN("failed to get tablet meta info", K(ret), K(tenant_id), K(tablet_id), K(ls_id), K(src_addr));
  } else if (OB_FAIL(compaction_scn.convert_for_tx(tablet_replica.get_snapshot_version()))) {
    LOG_WARN("failed to get tablet meta info", K(ret), K(compaction_scn), K(tenant_id), K(tablet_id), K(ls_id), K(src_addr));
  } else {/*do nothing*/}
  return ret;
}

int ObStorageHAUtils::check_tablet_replica_checksum_(const uint64_t tenant_id, const common::ObTabletID &tablet_id,
    const share::ObLSID &ls_id, const SCN &compaction_scn, common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObArray<ObTabletReplicaChecksumItem> items;
  ObArray<ObTabletLSPair> pairs;
  ObTabletLSPair pair;
  if (OB_FAIL(pair.init(tablet_id, ls_id))) {
    LOG_WARN("failed to init pair", K(ret), K(tablet_id), K(ls_id));
  } else if (OB_FAIL(pairs.push_back(pair))) {
    LOG_WARN("failed to push back", K(ret), K(pair));
  } else if (OB_FAIL(ObTabletReplicaChecksumOperator::batch_get(tenant_id, pairs, compaction_scn, sql_client, items))) {
    LOG_WARN("failed to batch get replica checksum item", K(ret), K(tenant_id), K(pairs), K(compaction_scn));
  } else {
    ObArray<share::ObTabletReplicaChecksumItem> filter_items;
    for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
      const ObTabletReplicaChecksumItem &item = items.at(i);
      if (item.compaction_scn_ == compaction_scn) {
        if (OB_FAIL(filter_items.push_back(item))) {
          LOG_WARN("failed to push back", K(ret), K(item));
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < filter_items.count(); ++i) {
      const ObTabletReplicaChecksumItem &first_item = filter_items.at(0);
      const ObTabletReplicaChecksumItem &item = filter_items.at(i);
      if (OB_FAIL(first_item.verify_checksum(item))) {
        LOG_ERROR("failed to verify checksum", K(ret), K(tenant_id), K(tablet_id),
            K(ls_id), K(compaction_scn), K(first_item), K(item), K(filter_items));
      }
    }
  }
  return ret;
}

int ObStorageHAUtils::check_ls_deleted(
    const share::ObLSID &ls_id,
    bool &is_deleted)
{
  int ret = OB_SUCCESS;
  const int64_t tenant_id = MTL_ID();
  ObLSExistState state = ObLSExistState::MAX_STATE;
  is_deleted = false;

  // sys tenant should always return LS_NORMAL
  if (!ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get ls status from inner table get invalid argument", K(ret), K(ls_id));
  } else if (!REACH_TENANT_TIME_INTERVAL(60 * 1000L * 1000L)) { //60s
    is_deleted = false;
  } else if (OB_FAIL(ObLocationService::check_ls_exist(tenant_id, ls_id, state))) {
    LOG_WARN("failed to check ls exist", K(ret), K(tenant_id), K(ls_id));
    //overwrite ret
    is_deleted = false;
    ret = OB_SUCCESS;
  } else if (state.is_deleted()) {
    is_deleted = true;
  } else {
    is_deleted = false;
  }
  return ret;
}

int ObStorageHAUtils::check_transfer_ls_can_rebuild(
    const share::SCN replay_scn,
    bool &need_rebuild)
{
  int ret = OB_SUCCESS;
  SCN readable_scn = SCN::base_scn();
  need_rebuild = false;
  if (!replay_scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument invalid", K(ret), K(replay_scn));
  } else if (MTL_TENANT_ROLE_CACHE_IS_INVALID()) {
    ret = OB_NEED_RETRY;
    LOG_WARN("tenant role is invalid, need retry", KR(ret), K(replay_scn));
  } else if (MTL_TENANT_ROLE_CACHE_IS_PRIMARY()) {
    need_rebuild = true;
  } else if (OB_FAIL(get_readable_scn_(readable_scn))) {
    LOG_WARN("failed to get readable scn", K(ret), K(replay_scn));
  } else if (readable_scn >= replay_scn) {
    need_rebuild = true;
  } else {
    need_rebuild = false;
  }
  return ret;
}

int ObStorageHAUtils::get_readable_scn_with_retry(share::SCN &readable_scn)
{
  int ret = OB_SUCCESS;
  readable_scn.set_base();
  rootserver::ObTenantInfoLoader *info = MTL(rootserver::ObTenantInfoLoader*);
  const int64_t GET_READABLE_SCN_INTERVAL = 100 * 1000; // 100ms
  const int64_t GET_REABLE_SCN_TIMEOUT = 9 * 1000 * 1000; // 9s

  if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant info is null", K(ret), KP(info));
  } else {
    const int64_t start_ts = ObTimeUtility::current_time();
    while (OB_SUCC(ret)) {
      if (OB_FAIL(get_readable_scn_(readable_scn))) {
        LOG_WARN("failed to get readable scn", K(ret));
        if (OB_EAGAIN == ret) {
          //overwrite ret
          if (ObTimeUtil::current_time() - start_ts >= GET_REABLE_SCN_TIMEOUT) {
            ret = OB_TIMEOUT;
            LOG_WARN("get valid readable scn timeout", K(ret), K(readable_scn));
          } else {
            ret = OB_SUCCESS;
            ob_usleep(GET_READABLE_SCN_INTERVAL);
          }
        }
      } else {
        break;
      }
    }
  }
  return ret;
}

int ObStorageHAUtils::get_readable_scn_(share::SCN &readable_scn)
{
  int ret = OB_SUCCESS;
  readable_scn.set_base();
  rootserver::ObTenantInfoLoader *info = MTL(rootserver::ObTenantInfoLoader*);
  if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant info is null", K(ret), KP(info));
  } else if (OB_FAIL(info->get_readable_scn(readable_scn))) {
    LOG_WARN("failed to get readable scn", K(ret), K(readable_scn));
  } else if (!readable_scn.is_valid()) {
    ret = OB_EAGAIN;
    LOG_WARN("readable_scn not valid", K(ret), K(readable_scn));
  }
  return ret;
}

int ObStorageHAUtils::check_is_primary_tenant(const uint64_t tenant_id, bool &is_primary_tenant)
{
  int ret = OB_SUCCESS;
  is_primary_tenant = false;
  if (OB_INVALID_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("check is primary tenant", K(ret), K(tenant_id));
  } else if (OB_FAIL(ObAllTenantInfoProxy::is_primary_tenant(GCTX.sql_proxy_, tenant_id, is_primary_tenant))) {
    LOG_WARN("check is standby tenant failed", K(ret), K(tenant_id));
  }
  return ret;
}

int ObStorageHAUtils::check_disk_space()
{
  int ret = OB_SUCCESS;
  const int64_t required_size = 0;
  if (OB_FAIL(THE_IO_DEVICE->check_space_full(required_size))) {
    LOG_WARN("failed to check is disk full, cannot transfer in", K(ret));
  }
  return ret;
}

int ObStorageHAUtils::calc_tablet_sstable_macro_block_cnt(
    const ObTabletHandle &tablet_handle, int64_t &data_macro_block_count)
{
  int ret = OB_SUCCESS;
  data_macro_block_count = 0;
  storage::ObTableStoreIterator table_store_iter;
  if (OB_UNLIKELY(!tablet_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid tablet handle", K(ret), K(tablet_handle));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_all_sstables(table_store_iter))) {
    LOG_WARN("failed to get all tables", K(ret), K(tablet_handle));
  } else if (0 == table_store_iter.count()) {
    // do nothing
  } else {
    ObITable *table_ptr = NULL;
    while (OB_SUCC(ret)) {
      table_ptr = NULL;
      if (OB_FAIL(table_store_iter.get_next(table_ptr))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else {
          LOG_WARN("failed to get next", K(ret));
        }
      } else if (OB_ISNULL(table_ptr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table should not be null", K(ret));
      } else if (!table_ptr->is_sstable()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("table is not sstable", K(ret), KPC(table_ptr));
      } else {
        data_macro_block_count += static_cast<blocksstable::ObSSTable *>(table_ptr)->get_data_macro_block_count();
      }
    }
  }
  return ret;
}

int ObStorageHAUtils::check_ls_is_leader(
    const uint64_t tenant_id,
    const share::ObLSID &ls_id,
    bool &is_leader)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_srv = NULL;
  common::ObRole role = common::ObRole::INVALID_ROLE;
  int64_t proposal_id = 0;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  is_leader = false;
  if (OB_INVALID_ID == tenant_id || !ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ls_id));
  } else if (OB_ISNULL(ls_srv = MTL_WITH_CHECK_TENANT(ObLSService *, tenant_id))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("log stream service is NULL", K(ret), K(tenant_id));
  } else if (OB_FAIL(ls_srv->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    LOG_WARN("ls should not be null", K(ret), KP(ls));
  } else if (OB_FAIL(ls->get_log_handler()->get_role(role, proposal_id))) {
    LOG_WARN("failed to get role", K(ret), KP(ls));
  } else if (is_strong_leader(role)) {
    is_leader = true;
  } else {
    is_leader = false;
  }
  return ret;
}

bool ObTransferUtils::is_need_retry_error(const int err)
{
  bool bool_ret = false;
  //white list
  switch (err) {
  //Has active trans need retry
  case OB_TRANSFER_MEMBER_LIST_NOT_SAME:
  case OB_LS_LOCATION_LEADER_NOT_EXIST:
  case OB_PARTITION_NOT_LEADER:
  case OB_TRANS_TIMEOUT:
  case OB_TIMEOUT:
  case OB_EAGAIN:
  case OB_ERR_EXCLUSIVE_LOCK_CONFLICT:
      bool_ret = true;
      break;
    default:
      break;
  }
  return bool_ret;
}

int ObTransferUtils::block_tx(const uint64_t tenant_id, const share::ObLSID &ls_id, const share::SCN &gts)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_svr = NULL;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;

  if (OB_INVALID_ID == tenant_id || !ls_id.is_valid() || !gts.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("block tx get invalid argument", K(ret), K(tenant_id), K(ls_id), K(gts));
  } else if (OB_ISNULL(ls_svr = (MTL(ObLSService *)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls service should not be NULL", K(ret), KP(ls_svr));
  } else if (OB_FAIL(ls_svr->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))) {
    LOG_WARN("ls_srv->get_ls() fail", K(ret), K(ls_id));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", KR(ret), K(ls_handle));
  } else if (OB_FAIL(ls->ha_block_tx(gts))) {
    LOG_WARN("failed to kill all tx", K(ret), KPC(ls));
  } else {
    LOG_INFO("success to kill all tx", K(ret), K(gts));
  }
  return ret;
}

// TODO(yangyi.yyy): get gts before block and kill tx, unblock no need get gts
int ObTransferUtils::kill_tx(const uint64_t tenant_id, const share::ObLSID &ls_id, const share::SCN &gts)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_svr = NULL;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  if (OB_INVALID_ID == tenant_id || !ls_id.is_valid() || !gts.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("block tx get invalid argument", K(ret), K(tenant_id), K(ls_id), K(gts));
  } else if (OB_ISNULL(ls_svr = (MTL(ObLSService *)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls service should not be NULL", K(ret), KP(ls_svr));
  } else if (OB_ISNULL(ls_svr = MTL(ObLSService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls srv should not be NULL", K(ret), KP(ls_svr));
  } else if (OB_FAIL(ls_svr->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))) {
    LOG_WARN("ls_srv->get_ls() fail", K(ret), K(ls_id));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", KR(ret), K(ls_handle));
  } else if (OB_FAIL(ls->ha_kill_tx(gts))) {
    LOG_WARN("failed to kill all tx", K(ret), KPC(ls));
  } else {
    LOG_INFO("success to kill all tx", K(ret), K(tenant_id), K(ls_id));
  }
  return ret;
}

int ObTransferUtils::unblock_tx(const uint64_t tenant_id, const share::ObLSID &ls_id, const share::SCN &gts)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObLSService *ls_srv = NULL;
  ObLS *ls = NULL;

  if (OB_INVALID_ID == tenant_id || !ls_id.is_valid() || !gts.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("block tx get invalid argument", K(ret), K(tenant_id), K(ls_id), K(gts));
  } else if (OB_ISNULL(ls_srv = MTL(ObLSService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls srv should not be NULL", K(ret), KP(ls_srv));
  } else if (OB_FAIL(ls_srv->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))) {
    LOG_WARN("ls_srv->get_ls() fail", K(ret), K(ls_id));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", KR(ret), K(ls_handle));
  } else if (OB_FAIL(ls->ha_unblock_tx(gts))) {
    LOG_WARN("failed to unblock tx", K(ret), K(tenant_id), K(ls_id), K(gts));
  }
  return ret;
}

int ObTransferUtils::get_gts(const uint64_t tenant_id, SCN &gts)
{
  int ret = OB_SUCCESS;
  if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant id is invalid", K(ret), K(tenant_id));
  } else {
    ret = OB_EAGAIN;
    const transaction::MonotonicTs stc = transaction::MonotonicTs::current_time();
    transaction::MonotonicTs unused_ts(0);
    const int64_t start_time = ObTimeUtility::fast_current_time();
    const int64_t TIMEOUT = 10 * 1000 * 1000; //10s
    while (OB_EAGAIN == ret) {
      if (ObTimeUtility::fast_current_time() - start_time > TIMEOUT) {
        ret = OB_TIMEOUT;
        LOG_WARN("get gts timeout", KR(ret), K(start_time), K(TIMEOUT));
      } else if (OB_FAIL(OB_TS_MGR.get_gts(tenant_id, stc, NULL, gts, unused_ts))) {
        if (OB_EAGAIN != ret) {
          LOG_WARN("failed to get gts", KR(ret), K(tenant_id));
        } else {
          // waiting 10ms
          ob_usleep(10L * 1000L);
        }
      }
    }
  }
  LOG_INFO("get tenant gts", KR(ret), K(tenant_id), K(gts));
  return ret;
}

int64_t ObStorageHAUtils::get_rpc_timeout()
{
  int64_t rpc_timeout = ObStorageRpcProxy::STREAM_RPC_TIMEOUT;
  int64_t tmp_rpc_timeout = 0;
  omt::ObTenantConfigGuard tenant_config(TENANT_CONF(MTL_ID()));
  if (tenant_config.is_valid()) {
    tmp_rpc_timeout = tenant_config->_ha_rpc_timeout;
    rpc_timeout = std::max(rpc_timeout, tmp_rpc_timeout);
  }
  return rpc_timeout;
}

int ObStorageHAUtils::check_log_need_rebuild(const uint64_t tenant_id, const share::ObLSID &ls_id, bool &need_rebuild)
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  common::ObAddr parent_addr;
  ObLSHandle ls_handle;
  bool is_log_sync = false;

  if (OB_INVALID_TENANT_ID == tenant_id || !ls_id.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument is not valid", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(ObStorageHADagUtils::get_ls(ls_id, ls_handle))) {
    LOG_WARN("failed to get ls", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", K(ret), KP(ls), K(tenant_id), K(ls_id));
  } else if (OB_ISNULL(ls->get_log_handler())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("log handler should not be NULL", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(ls->get_log_handler()->is_in_sync(is_log_sync, need_rebuild))) {
    LOG_WARN("failed to get is_in_sync", K(ret), K(tenant_id), K(ls_id));
  }

#ifdef ERRSIM
  if (OB_SUCC(ret)) {
    int tmp_ret = OB_SUCCESS;
    tmp_ret = EN_CHECK_LOG_NEED_REBUILD ? : OB_SUCCESS;
    if (OB_TMP_FAIL(tmp_ret)) {
      need_rebuild = true;
      SERVER_EVENT_ADD("storage_ha", "check_log_need_rebuild",
                      "tenant_id", tenant_id,
                      "ls_id", ls_id.id(),
                      "result", tmp_ret);
      DEBUG_SYNC(AFTER_CHECK_LOG_NEED_REBUILD);
    }
  }
#endif
  return ret;
}

void ObTransferUtils::set_transfer_module()
{
#ifdef ERRSIM
  if (ObErrsimModuleType::ERRSIM_MODULE_NONE == THIS_WORKER.get_module_type().type_) {
    ObErrsimModuleType type(ObErrsimModuleType::ERRSIM_MODULE_TRANSFER);
    THIS_WORKER.set_module_type(type);
  }
#endif
}

void ObTransferUtils::clear_transfer_module()
{
#ifdef ERRSIM
  if (ObErrsimModuleType::ERRSIM_MODULE_TRANSFER == THIS_WORKER.get_module_type().type_) {
    ObErrsimModuleType type(ObErrsimModuleType::ERRSIM_MODULE_NONE);
    THIS_WORKER.set_module_type(type);
  }
#endif
}

void ObTransferUtils::transfer_tablet_restore_stat(
    const uint64_t tenant_id,
    const share::ObLSID &src_ls_id,
    const share::ObLSID &dest_ls_id)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  ObLSService *ls_service = nullptr;
  ObLSHandle src_ls_handle;
  ObLSHandle dest_ls_handle;
  ObLS *src_ls = nullptr;
  ObLS *dest_ls = nullptr;
  ObRestorePersistHelper helper;
  common::ObMySQLProxy *sql_proxy = nullptr;
  ObArray<ObLSRestoreProgressPersistInfo> ls_restore_progress_array;
  if (OB_ISNULL(ls_service = MTL(ObLSService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get ObLSService from MTL", K(ret), KP(ls_service));
  } else if (OB_FAIL(ls_service->get_ls(dest_ls_id, dest_ls_handle, ObLSGetMod::HA_MOD))) {
    LOG_WARN("failed to get ls", K(ret), K(dest_ls_id));
  } else if (OB_ISNULL(dest_ls = dest_ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dest ls should not be NULL", K(ret), K(dest_ls_handle));
  } else if (OB_FAIL(ls_service->get_ls(src_ls_id, src_ls_handle, ObLSGetMod::HA_MOD))) {
    LOG_WARN("failed to get ls", K(ret), K(src_ls_id));
  } else if (OB_ISNULL(src_ls = src_ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("src ls should not be NULL", K(ret), K(src_ls_handle));
  } else if (OB_ISNULL(sql_proxy = GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql prxoy must not be null", K(ret));
  } else if (OB_FAIL(helper.init(tenant_id))) {
    LOG_WARN("fail to init restore table helper", K(ret));
  } else if (OB_FAIL(trans.start(sql_proxy, gen_meta_tenant_id(tenant_id)))) {
    LOG_WARN("fail to start trans", K(ret), K(tenant_id));
  } else {
    ObLSRestoreJobPersistKey src_ls_key;
    ObLSRestoreJobPersistKey dest_ls_key;
    if (OB_FAIL(helper.get_all_ls_restore_progress(trans, ls_restore_progress_array))) {
      LOG_WARN("fail to get all ls restore progress", K(ret));
    } else if (ls_restore_progress_array.empty()) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("fail to get all ls restore progress", K(ret));
    } else {
      dest_ls_key.tenant_id_ = tenant_id;
      dest_ls_key.job_id_ = ls_restore_progress_array.at(0).key_.job_id_;
      dest_ls_key.ls_id_ = dest_ls_id;
      dest_ls_key.addr_ = GCTX.self_addr();

      src_ls_key = dest_ls_key;
      src_ls_key.ls_id_ = src_ls_id;
      if (OB_FAIL(helper.transfer_tablet(trans, src_ls_key, dest_ls_key))) {
        LOG_WARN("fail to transfer tablet restore stat", K(ret), K(src_ls_key), K(dest_ls_key));
      }
    }

    if (trans.is_started()) {
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
        LOG_WARN("failed to commit trans", K(ret), K(tmp_ret));
        ret = OB_SUCC(ret) ? tmp_ret : ret;
      }
    }

    if (FAILEDx(dest_ls->get_ls_restore_handler()->restore_stat().inc_total_tablet_cnt())) {
      LOG_WARN("fail to inc dest ls total tablet cnt", K(ret), K(dest_ls_key));
    } else if (OB_FAIL(src_ls->get_ls_restore_handler()->restore_stat().dec_total_tablet_cnt())) {
      LOG_WARN("fail to inc dest ls total tablet cnt", K(ret), K(src_ls_key));
    }
  }
}
} // end namespace storage
} // end namespace oceanbase
