/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include <chrono>
#include <memory>
#include <string.h>
#include <time.h>

#include <gtest/gtest.h>
#include <folly/io/async/EventBaseManager.h>

#include "MagmaService.h"
#include "ProtobufCreators.h"
#include "ServiceRegistrySingleton.h"
#include "SessiondMocks.h"
#include "LocalEnforcer.h"
#include "magma_logging.h"

#define SECONDS_A_DAY 86400

using ::testing::Test;
using grpc::Status;
using grpc::ServerContext;

namespace magma {

const SessionState::Config test_cfg =
    {.ue_ipv4 = "127.0.0.1", .spgw_ipv4 = "128.0.0.1"};

class LocalEnforcerTest : public ::testing::Test {
protected:

protected:
  virtual void SetUp() {
    rule_store = std::make_shared<StaticRuleStore>();
    pipelined_client = std::make_shared<MockPipelinedClient>();
    local_enforcer = std::make_unique<LocalEnforcer>(
      rule_store,
      pipelined_client);
  }

  void insert_static_rule(
      uint32_t rating_group,
      const std::string& m_key,
      const std::string& rule_id) {
    PolicyRule rule;
    rule.set_id(rule_id);
    rule.set_rating_group(rating_group);
    rule.set_monitoring_key(m_key);
    if (rating_group == 0 && m_key.length() > 0) {
      rule.set_tracking_type(PolicyRule::ONLY_PCRF);
    } else if (rating_group > 0 && m_key.length() == 0) {
      rule.set_tracking_type(PolicyRule::ONLY_OCS);
    } else if (rating_group > 0 && m_key.length() > 0) {
      rule.set_tracking_type(PolicyRule::OCS_AND_PCRF);
    } else {
      rule.set_tracking_type(PolicyRule::NO_TRACKING);
    }
    rule_store->insert_rule(rule);
  }

  void assert_charging_credit(
      const std::string& imsi,
      Bucket bucket,
      const std::vector<std::pair<uint32_t, uint64_t>>& volumes) {
    for (auto& volume_pair : volumes) {
      auto volume_out = local_enforcer->get_charging_credit(
        imsi, volume_pair.first, bucket);
      EXPECT_EQ(volume_out, volume_pair.second);
    }
  }

  void assert_monitor_credit(
      const std::string& imsi,
      Bucket bucket,
      const std::vector<std::pair<std::string, uint64_t>>& volumes) {
    for (auto& volume_pair : volumes) {
      auto volume_out = local_enforcer->get_monitor_credit(
        imsi, volume_pair.first, bucket);
      EXPECT_EQ(volume_out, volume_pair.second);
    }
  }

protected:
  std::shared_ptr<StaticRuleStore> rule_store;
  std::unique_ptr<LocalEnforcer> local_enforcer;
  std::shared_ptr<MockPipelinedClient> pipelined_client;
};

MATCHER_P(CheckCount, count, "") {
  return arg.size() == count;
}

MATCHER_P2(CheckActivateFlows, imsi, rule_count, "") {
  auto request = static_cast<const ActivateFlowsRequest*>(arg);
  return request->sid().id() == imsi && request->rule_ids_size() == rule_count;
}

TEST_F(LocalEnforcerTest, test_init_session_credit) {
  insert_static_rule(1, "", "rule1");

  CreateSessionResponse response;
  auto credits = response.mutable_credits();
  create_update_response("IMSI1", 1, 1024, credits->Add());

  EXPECT_CALL(*pipelined_client,
    activate_flows_for_rules(testing::_, testing::_, CheckCount(0), CheckCount(0)))
    .Times(1)
    .WillOnce(testing::Return(true));
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 1024);
}

TEST_F(LocalEnforcerTest, test_single_record) {
  // insert initial session credit
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  insert_static_rule(1, "", "rule1");
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 16, 32, record_list->Add());

  local_enforcer->aggregate_records(table);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_RX), 16);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_TX), 32);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 1024);
}

TEST_F(LocalEnforcerTest, test_aggregate_records) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  create_update_response("IMSI1", 2, 1024, response.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  insert_static_rule(1, "", "rule1");
  insert_static_rule(1, "", "rule2");
  insert_static_rule(2, "", "rule3");
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 10, 20, record_list->Add());
  create_rule_record("IMSI1", "rule2", 5, 15, record_list->Add());
  create_rule_record("IMSI1", "rule3", 100, 150, record_list->Add());

  local_enforcer->aggregate_records(table);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_RX), 15);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_TX), 35);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 2, USED_RX), 100);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 2, USED_TX), 150);
}

TEST_F(LocalEnforcerTest, test_collect_updates) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  insert_static_rule(1, "", "rule1");

  auto empty_update = local_enforcer->collect_updates();
  EXPECT_EQ(empty_update.updates_size(), 0);

  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 1024, 2048, record_list->Add());

  local_enforcer->aggregate_records(table);
  auto session_update = local_enforcer->collect_updates();
  EXPECT_EQ(session_update.updates_size(), 1);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, REPORTING_RX), 1024);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, REPORTING_TX), 2048);
}

TEST_F(LocalEnforcerTest, test_update_session_credit) {
  insert_static_rule(1, "", "rule1");

  CreateSessionResponse response;
  auto credits = response.mutable_credits();
  create_update_response("IMSI1", 1, 1024, credits->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 1024);

  UpdateSessionResponse update_response;
  auto updates = update_response.mutable_responses();
  create_update_response("IMSI1", 1, 24, updates->Add());
  local_enforcer->update_session_credit(update_response);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 1048);
}

TEST_F(LocalEnforcerTest, test_terminate_credit) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  create_update_response("IMSI1", 2, 2048, response.mutable_credits()->Add());
  CreateSessionResponse response2;
  create_update_response("IMSI2", 1, 4096, response2.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  local_enforcer->init_session_credit("IMSI2", "4321", test_cfg, response2);

  auto req = local_enforcer->terminate_subscriber("IMSI1");
  EXPECT_EQ(req.credit_usages_size(), 2);
  for (const auto& usage : req.credit_usages()) {
    EXPECT_EQ(usage.type(), CreditUsage::TERMINATED);
  }
  local_enforcer->complete_termination("IMSI1", "1234");
  // No longer in system
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 0);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 2, ALLOWED_TOTAL), 0);
}

TEST_F(LocalEnforcerTest, test_terminate_credit_during_reporting) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  create_update_response("IMSI1", 2, 2048, response.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  insert_static_rule(1, "", "rule1");
  insert_static_rule(2, "", "rule2");

  // Insert record for key 1
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 1024, 2048, record_list->Add());
  local_enforcer->aggregate_records(table);

  // Collect updates to put key 1 into reporting state
  auto usage_updates = local_enforcer->collect_updates();
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, REPORTING_RX), 1024);

  // Collecting terminations should key 1 anyways during reporting
  auto term_req = local_enforcer->terminate_subscriber("IMSI1");
  EXPECT_EQ(term_req.credit_usages_size(), 2);
}

MATCHER_P2(CheckDeactivateFlows, imsi, rule_count, "") {
  auto request = static_cast<const DeactivateFlowsRequest*>(arg);
  return request->sid().id() == imsi && request->rule_ids_size() == rule_count;
}

TEST_F(LocalEnforcerTest, test_final_unit_handling) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, true, 1024,
    response.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  insert_static_rule(1, "", "rule1");
  insert_static_rule(1, "", "rule2");

  // Insert record for key 1
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 1024, 2048, record_list->Add());
  create_rule_record("IMSI1", "rule2", 1024, 2048, record_list->Add());
  local_enforcer->aggregate_records(table);

  EXPECT_CALL(*pipelined_client,
    deactivate_flows_for_rules(testing::_, testing::_, testing::_))
    .Times(1)
    .WillOnce(testing::Return(true));
  // call collect_updates to trigger actions
  auto usage_updates = local_enforcer->collect_updates();
}

TEST_F(LocalEnforcerTest, test_all) {
  // insert key rule mapping
  insert_static_rule(1, "", "rule1");
  insert_static_rule(1, "", "rule2");
  insert_static_rule(2, "", "rule3");

  // insert initial session credit
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  CreateSessionResponse response2;
  create_update_response("IMSI2", 2, 1024, response2.mutable_credits()->Add());
  local_enforcer->init_session_credit("IMSI2", "4321", test_cfg, response2);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 1024);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, ALLOWED_TOTAL), 1024);

  // receive usages from pipelined
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 10, 20, record_list->Add());
  create_rule_record("IMSI1", "rule2", 5, 15, record_list->Add());
  create_rule_record("IMSI2", "rule3", 1024, 1024, record_list->Add());
  local_enforcer->aggregate_records(table);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_RX), 15);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_TX), 35);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, USED_RX), 1024);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, USED_TX), 1024);

  // Collect updates for reporting
  auto session_update = local_enforcer->collect_updates();
  EXPECT_EQ(session_update.updates_size(), 1);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, REPORTING_RX), 1024);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, REPORTING_TX), 1024);

  // Add updated credit from cloud
  UpdateSessionResponse update_response;
  auto updates = update_response.mutable_responses();
  create_update_response("IMSI2", 2, 4096, updates->Add());
  local_enforcer->update_session_credit(update_response);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, ALLOWED_TOTAL), 5120);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, REPORTING_TX), 0);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, REPORTING_RX), 0);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, REPORTED_TX), 1024);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI2", 2, REPORTED_RX), 1024);
  // Terminate IMSI1
  auto req = local_enforcer->terminate_subscriber("IMSI1");
  EXPECT_EQ(req.sid(), "IMSI1");
  EXPECT_EQ(req.credit_usages_size(), 1);
}

TEST_F(LocalEnforcerTest, test_re_auth) {
  insert_static_rule(1, "", "rule1");
  CreateSessionResponse response;
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  ChargingReAuthRequest reauth;
  reauth.set_sid("IMSI1");
  reauth.set_charging_key(1);
  reauth.set_type(ChargingReAuthRequest::SINGLE_SERVICE);
  auto result = local_enforcer->init_charging_reauth(reauth);
  EXPECT_EQ(result, ChargingReAuthAnswer::UPDATE_INITIATED);

  auto update_req = local_enforcer->collect_updates();
  EXPECT_EQ(update_req.updates_size(), 1);
  EXPECT_EQ(update_req.updates(0).sid(), "IMSI1");
  EXPECT_EQ(update_req.updates(0).usage().type(), CreditUsage::REAUTH_REQUIRED);

  // Give credit after re-auth
  UpdateSessionResponse update_response;
  auto updates = update_response.mutable_responses();
  create_update_response("IMSI1", 1, 4096, updates->Add());
  local_enforcer->update_session_credit(update_response);

  // when next update is collected, this should trigger an action to activate
  // the flow in pipelined
  EXPECT_CALL(*pipelined_client,
    activate_flows_for_rules(testing::_, testing::_, testing::_, testing::_))
    .Times(1)
    .WillOnce(testing::Return(true));
  local_enforcer->collect_updates();
}

TEST_F(LocalEnforcerTest, test_dynamic_rules) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  auto dynamic_rule = response.mutable_dynamic_rules()->Add();
  auto policy_rule = dynamic_rule->mutable_policy_rule();
  policy_rule->set_id("rule1");
  policy_rule->set_rating_group(1);
  policy_rule->set_tracking_type(PolicyRule::ONLY_OCS);
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  insert_static_rule(1, "", "rule2");
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 16, 32, record_list->Add());
  create_rule_record("IMSI1", "rule2", 8, 8, record_list->Add());

  local_enforcer->aggregate_records(table);

  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_RX), 24);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, USED_TX), 40);
  EXPECT_EQ(local_enforcer->get_charging_credit("IMSI1", 1, ALLOWED_TOTAL), 1024);
}

TEST_F(LocalEnforcerTest, test_dynamic_rule_actions) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, true, 1024,
    response.mutable_credits()->Add());
  auto dynamic_rule = response.mutable_dynamic_rules()->Add();
  auto policy_rule = dynamic_rule->mutable_policy_rule();
  policy_rule->set_id("rule2");
  policy_rule->set_rating_group(1);
  policy_rule->set_tracking_type(PolicyRule::ONLY_OCS);
  insert_static_rule(1, "", "rule1");
  insert_static_rule(1, "", "rule3");

  EXPECT_CALL(*pipelined_client,
    activate_flows_for_rules(testing::_, testing::_, CheckCount(0), CheckCount(1)))
    .Times(1)
    .WillOnce(testing::Return(true));
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);

  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "rule1", 1024, 2048, record_list->Add());
  create_rule_record("IMSI1", "rule2", 1024, 2048, record_list->Add());
  local_enforcer->aggregate_records(table);

  EXPECT_CALL(*pipelined_client,
    deactivate_flows_for_rules(testing::_, CheckCount(2), CheckCount(1)))
    .Times(1)
    .WillOnce(testing::Return(true));
  auto usage_updates = local_enforcer->collect_updates();
}

TEST_F(LocalEnforcerTest, test_installing_rules_with_activation_time) {
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, true, 1024,
    response.mutable_credits()->Add());

  // add a dynamic rule without activation time
  auto dynamic_rule = response.mutable_dynamic_rules()->Add();
  auto policy_rule = dynamic_rule->mutable_policy_rule();
  policy_rule->set_id("rule1");
  policy_rule->set_rating_group(1);
  policy_rule->set_tracking_type(PolicyRule::ONLY_OCS);

  // add a dynamic rule with activation time in the future
  dynamic_rule = response.mutable_dynamic_rules()->Add();
  policy_rule = dynamic_rule->mutable_policy_rule();
  auto activation_time = dynamic_rule->mutable_activation_time();
  activation_time->set_seconds(time(NULL) + SECONDS_A_DAY);
  policy_rule->set_id("rule2");
  policy_rule->set_rating_group(1);
  policy_rule->set_tracking_type(PolicyRule::ONLY_OCS);

  // add a dynamic rule with activation time in the past
  dynamic_rule = response.mutable_dynamic_rules()->Add();
  policy_rule = dynamic_rule->mutable_policy_rule();
  activation_time = dynamic_rule->mutable_activation_time();
  activation_time->set_seconds(time(NULL) - SECONDS_A_DAY);
  policy_rule->set_id("rule3");
  policy_rule->set_rating_group(1);
  policy_rule->set_tracking_type(PolicyRule::ONLY_OCS);

  // add a static rule without activation time
  insert_static_rule(1, "", "rule4");
  auto static_rule = response.mutable_static_rules()->Add();
  static_rule->set_rule_id("rule4");

  // add a static rule with activation time in the future
  insert_static_rule(1, "", "rule5");
  static_rule = response.mutable_static_rules()->Add();
  activation_time = static_rule->mutable_activation_time();
  activation_time->set_seconds(time(NULL) + SECONDS_A_DAY);
  static_rule->set_rule_id("rule5");

  // add a static rule with activation time in the past
  insert_static_rule(1, "", "rule6");
  static_rule = response.mutable_static_rules()->Add();
  activation_time = static_rule->mutable_activation_time();
  activation_time->set_seconds(time(NULL) - SECONDS_A_DAY);
  static_rule->set_rule_id("rule6");

  folly::EventBase* evb = folly::EventBaseManager::get()->getEventBase();
  local_enforcer->attachEventBase(evb);

  // expect calling activate_flows_for_rules for activating rules instantly
  // dynamic rules: rule1, rule3
  // static rules: rule4, rule6
  EXPECT_CALL(*pipelined_client,
    activate_flows_for_rules(
      testing::_, testing::_, CheckCount(2), CheckCount(2)))
    .Times(1)
    .WillOnce(testing::Return(true));
  // expect calling activate_flows_for_rules for activating a static rule later
  // static rules: rule5
  EXPECT_CALL(*pipelined_client,
    activate_flows_for_rules(
      testing::_, testing::_, CheckCount(1), CheckCount(0)))
    .Times(1)
    .WillOnce(testing::Return(true));
  // expect calling activate_flows_for_rules for activating a dynamic rule later
  // dynamic rules: rule2
  EXPECT_CALL(*pipelined_client,
    activate_flows_for_rules(
      testing::_, testing::_, CheckCount(0), CheckCount(1)))
    .Times(1)
    .WillOnce(testing::Return(true));
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  delete evb;
}

TEST_F(LocalEnforcerTest, test_usage_monitors) {
  // insert key rule mapping
  insert_static_rule(1, "1", "both_rule");
  insert_static_rule(2, "", "ocs_rule");
  insert_static_rule(0, "3", "pcrf_only");
  insert_static_rule(0, "1", "pcrf_split"); // same mkey as both_rule
  // session level rule "4"

  // insert initial session credit
  CreateSessionResponse response;
  create_update_response("IMSI1", 1, 1024, response.mutable_credits()->Add());
  create_update_response("IMSI1", 2, 1024, response.mutable_credits()->Add());
  create_monitor_update_response(
    "IMSI1", "1", MonitoringLevel::PCC_RULE_LEVEL, 1024,
    response.mutable_usage_monitors()->Add());
  create_monitor_update_response(
    "IMSI1", "3", MonitoringLevel::PCC_RULE_LEVEL, 1024,
    response.mutable_usage_monitors()->Add());
  create_monitor_update_response(
    "IMSI1", "4", MonitoringLevel::SESSION_LEVEL, 1024,
    response.mutable_usage_monitors()->Add());
  local_enforcer->init_session_credit("IMSI1", "1234", test_cfg, response);
  assert_charging_credit("IMSI1", ALLOWED_TOTAL, {{1, 1024}, {2, 1024}});
  assert_monitor_credit("IMSI1", ALLOWED_TOTAL,
                        {{"1", 1024}, {"3", 1024}, {"4", 1024}});

  // receive usages from pipelined
  RuleRecordTable table;
  auto record_list = table.mutable_records();
  create_rule_record("IMSI1", "both_rule", 10, 20, record_list->Add());
  create_rule_record("IMSI1", "ocs_rule", 5, 15, record_list->Add());
  create_rule_record("IMSI1", "pcrf_only", 1024, 1024, record_list->Add());
  create_rule_record("IMSI1", "pcrf_split", 10, 20, record_list->Add());
  local_enforcer->aggregate_records(table);

  assert_charging_credit("IMSI1", USED_RX, {{1, 10}, {2, 5}});
  assert_charging_credit("IMSI1", USED_TX, {{1, 20}, {2, 15}});
  assert_monitor_credit("IMSI1", USED_RX,
                        {{"1", 20}, {"3", 1024}, {"4", 1049}});
  assert_monitor_credit("IMSI1", USED_TX,
                        {{"1", 40}, {"3", 1024}, {"4", 1079}});

  // Collect updates, should only have mkeys 3 and 4
  auto session_update = local_enforcer->collect_updates();
  EXPECT_EQ(session_update.usage_monitors_size(), 2);
  for (const auto& monitor : session_update.usage_monitors()) {
    EXPECT_EQ(monitor.sid(), "IMSI1");
    if (monitor.update().monitoring_key() == "3") {
      EXPECT_EQ(monitor.update().level(), MonitoringLevel::PCC_RULE_LEVEL);
      EXPECT_EQ(monitor.update().bytes_rx(), 1024);
      EXPECT_EQ(monitor.update().bytes_tx(), 1024);
    } else if (monitor.update().monitoring_key() == "4") {
      EXPECT_EQ(monitor.update().level(), MonitoringLevel::SESSION_LEVEL);
      EXPECT_EQ(monitor.update().bytes_rx(), 1049);
      EXPECT_EQ(monitor.update().bytes_tx(), 1079);
    } else {
      EXPECT_TRUE(false);
    }
  }

  assert_charging_credit("IMSI1", REPORTING_RX, {{1, 0}, {2, 0}});
  assert_charging_credit("IMSI1", REPORTING_TX, {{1, 0}, {2, 0}});
  assert_monitor_credit("IMSI1", REPORTING_RX,
                        {{"1", 0}, {"3", 1024}, {"4", 1049}});
  assert_monitor_credit("IMSI1", REPORTING_TX,
                        {{"1", 0}, {"3", 1024}, {"4", 1079}});

  UpdateSessionResponse update_response;
  auto monitor_updates = update_response.mutable_usage_monitor_responses();
  create_monitor_update_response(
    "IMSI1", "3", MonitoringLevel::PCC_RULE_LEVEL, 2048,
    monitor_updates->Add());
  create_monitor_update_response(
    "IMSI1", "4", MonitoringLevel::SESSION_LEVEL, 2048,
    monitor_updates->Add());
  local_enforcer->update_session_credit(update_response);
  assert_monitor_credit("IMSI1", REPORTING_RX, {{"3", 0}, {"4", 0}});
  assert_monitor_credit("IMSI1", REPORTING_TX, {{"3", 0}, {"4", 0}});
  assert_monitor_credit("IMSI1", REPORTED_RX, {{"3", 1024}, {"4", 1049}});
  assert_monitor_credit("IMSI1", REPORTED_TX, {{"3", 1024}, {"4", 1079}});
  assert_monitor_credit("IMSI1", ALLOWED_TOTAL, {{"3", 3072}, {"4", 3072}});
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  FLAGS_logtostderr = 1;
  FLAGS_v = 10;
  return RUN_ALL_TESTS();
}

}
