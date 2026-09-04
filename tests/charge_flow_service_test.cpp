#include "core/application/business_numbers.h"
#include "core/application/admin_station_service.h"
#include "core/application/charge_flow_service.h"
#include "core/application/charging_repository.h"
#include "core/application/in_memory_user_account_repository.h"
#include "core/application/in_memory_admin_repository.h"
#include "core/application/wallet_service.h"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace ncs::core::application;

class TestRunner final {
public:
  void check(const bool condition, const std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures_;
    }
  }
  int result() const { return failures_ == 0 ? 0 : 1; }

private:
  int failures_ = 0;
};

constexpr std::int64_t kZgcStationId = 1;
constexpr int kDcFast = 1;

UserAccount makeAccount(const std::string &username, const std::string &phone) {
  UserAccount account;
  account.username = username;
  account.phone = phone;
  account.nickname = "用户";
  return account;
}

} // namespace

int main() {
  TestRunner tests;
  InMemoryUserAccountRepository accounts;
  InMemoryChargingRepository repository;
  BusinessNumbers numbers;

  const auto now =
      std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
  const auto second = std::chrono::seconds(1);

  WalletService wallet(repository, accounts, numbers);
  ChargeFlowService flows(repository, accounts, accounts, numbers, 60);

  UserAccount driver = makeAccount("driver_a", "13800139001");
  UserAccount broke = makeAccount("driver_b", "13800139002");
  UserAccount debtor = makeAccount("driver_c", "13800139003");
  UserAccount holder = makeAccount("driver_d", "13800139004");
  UserAccount queuer = makeAccount("driver_e", "13800139005");
  UserAccount frozenAfterReservation = makeAccount("driver_f", "13800139006");
  tests.check(accounts.create(driver) == AccountWriteResult::Success &&
                  accounts.create(broke) == AccountWriteResult::Success &&
                  accounts.create(debtor) == AccountWriteResult::Success &&
                  accounts.create(holder) == AccountWriteResult::Success &&
                  accounts.create(queuer) == AccountWriteResult::Success &&
                  accounts.create(frozenAfterReservation) ==
                      AccountWriteResult::Success,
              "test accounts are created");

  const auto recharged = wallet.recharge(driver.id, 10000, now);
  tests.check(recharged.ok() && recharged.value->balanceAfterCent == 10000,
              "driver recharge lands on wallet");

  const auto tooSmall = wallet.recharge(driver.id, 0, now);
  const auto tooLarge = wallet.recharge(driver.id, 1000001, now);
  tests.check(
      tooSmall.error == ncs::core::domain::ErrorCode::ValidationFailed &&
          tooLarge.error == ncs::core::domain::ErrorCode::ValidationFailed,
      "recharge bounds are enforced");

  const auto flow =
      flows.createFlow(driver.id, kZgcStationId, kDcFast, std::nullopt, now);
  tests.check(flow.ok() && flow.value->status == 20,
              "compatible idle device yields a quote");
  tests.check(flow.value->quote &&
                  flow.value->quote->totalPriceCentPerKwh == 135,
              "quote snapshot uses the regional tariff");
  tests.check(flow.value->version == 1, "new flow starts at version one");

  const auto duplicate =
      flows.createFlow(driver.id, kZgcStationId, kDcFast, std::nullopt, now);
  tests.check(duplicate.error == ncs::core::domain::ErrorCode::ActiveFlowExists,
              "active flow uniqueness blocks a second flow");

  const auto brokeFlow =
      flows.createFlow(broke.id, kZgcStationId, kDcFast, std::nullopt, now);
  tests.check(brokeFlow.error ==
                  ncs::core::domain::ErrorCode::InsufficientBalance,
              "minimum start balance blocks a new flow");

  const auto wrongQuote = flows.confirmQuote(
      driver.id, flow.value->flowNo, "QT-mismatch", flow.value->version, now);
  tests.check(wrongQuote.error ==
                  ncs::core::domain::ErrorCode::InvalidStateTransition,
              "unknown quote number is rejected");

  const auto staleConfirm = flows.confirmQuote(driver.id, flow.value->flowNo,
                                               flow.value->quote->quoteNo,
                                               flow.value->version + 1, now);
  tests.check(staleConfirm.error ==
                  ncs::core::domain::ErrorCode::VersionConflict,
              "stale flow version conflicts on confirmation");

  const auto confirmed = flows.confirmQuote(
      driver.id, flow.value->flowNo, flow.value->quote->quoteNo,
      flow.value->version, now + 5 * second);
  tests.check(confirmed.ok() && confirmed.value->status == 30 &&
                  confirmed.value->reservedUntil == 1788500005 + 900 &&
                  confirmed.value->version == 2,
              "quote confirmation reserves the device for fifteen minutes");

  const auto progressTooEarly =
      flows.progress(driver.id, flow.value->flowNo, now + 6 * second);
  tests.check(progressTooEarly.error ==
                  ncs::core::domain::ErrorCode::InvalidStateTransition,
              "progress before start is refused");

  tests.check(wallet.recharge(frozenAfterReservation.id, 2000, now).ok(),
              "reservation freeze test account is funded");
  const auto freezeFlow = flows.createFlow(frozenAfterReservation.id, 2,
                                           kDcFast, std::nullopt, now);
  const auto freezeConfirmation =
      flows.confirmQuote(frozenAfterReservation.id, freezeFlow.value->flowNo,
                         freezeFlow.value->quote->quoteNo,
                         freezeFlow.value->version, now + second);
  UserAccount frozenAccount;
  tests.check(
      freezeConfirmation.ok() &&
          accounts.updateStatus(frozenAfterReservation.id, 0, frozenAccount) ==
              AccountWriteResult::Success,
      "account is frozen after reserving a device");
  const auto frozenStart =
      flows.start(frozenAfterReservation.id, freezeFlow.value->flowNo,
                  freezeConfirmation.value->version, std::nullopt, std::nullopt,
                  now + 2 * second);
  tests.check(frozenStart.error == ncs::core::domain::ErrorCode::UserFrozen,
              "start rechecks account status after reservation");

  const auto staleStart =
      flows.start(driver.id, flow.value->flowNo, 9, std::nullopt, std::nullopt,
                  now + 6 * second);
  tests.check(staleStart.error == ncs::core::domain::ErrorCode::VersionConflict,
              "stale flow version conflicts on start");

  const auto started =
      flows.start(driver.id, flow.value->flowNo, confirmed.value->version,
                  std::nullopt, std::nullopt, now + 10 * second);
  tests.check(started.ok() && started.value->status == 40 &&
                  started.value->timeScale == 60 &&
                  started.value->powerWatt == 60000,
              "start enters charging with a time scale snapshot");

  const auto progress =
      flows.progress(driver.id, flow.value->flowNo, now + 20 * second);
  tests.check(progress.ok() && progress.value->durationSec == 600 &&
                  progress.value->energyMwh == 10000000 &&
                  progress.value->amountCent == 1350 &&
                  progress.value->simulatedSoc > 20,
              "progress derives simulated energy and amount from the snapshot");

  const auto settled =
      flows.settle(driver.id, flow.value->flowNo, started.value->version,
                   "USER_STOPPED", now + 70 * second);
  tests.check(settled.ok() && settled.value->durationSec == 3600 &&
                  settled.value->energyMwh == 60000000 &&
                  settled.value->amountCent == 8100 &&
                  settled.value->paidCent == 8100 &&
                  settled.value->debtAddedCent == 0 &&
                  settled.value->balanceAfterCent == 1900 &&
                  settled.value->status == 60,
              "settlement charges one simulated hour at the quote price");

  const auto overview = wallet.overview(driver.id);
  tests.check(overview.balanceCent == 1900 && overview.debtCent == 0,
              "wallet reflects the settlement deduction");
  const auto driverAfter = accounts.findById(driver.id);
  tests.check(driverAfter && !driverAfter->hasActiveFlow &&
                  driverAfter->balanceCent == 1900,
              "account mirror follows wallet and flow state");
  const auto driverFlow = flows.activeFlow(driver.id, now + 70 * second);
  tests.check(!driverFlow.hasActiveFlow,
              "completed flow leaves no active flow");

  // Debt path: a small balance settles into debt; recharges clear it first.
  tests.check(wallet.recharge(debtor.id, 600, now).ok(),
              "debtor seeds a small balance");
  const auto debtorFlow = flows.createFlow(debtor.id, kZgcStationId, kDcFast,
                                           std::nullopt, now + second);
  tests.check(debtorFlow.ok(), "debtor starts inside the minimum balance");
  const auto debtorConfirm = flows.confirmQuote(
      debtor.id, debtorFlow.value->flowNo, debtorFlow.value->quote->quoteNo,
      debtorFlow.value->version, now + 2 * second);
  const auto blockedByFloor = flows.start(
      debtor.id, debtorFlow.value->flowNo, debtorConfirm.value->version,
      std::nullopt, 1000, now + 2 * second);
  tests.check(
      blockedByFloor.error == ncs::core::domain::ErrorCode::InsufficientBalance,
      "client floor above the balance blocks start");
  const auto debtorStart = flows.start(
      debtor.id, debtorFlow.value->flowNo, debtorConfirm.value->version,
      std::nullopt, 0, now + 2 * second);
  tests.check(debtorStart.ok(), "zero client floor keeps the city minimum balance");
  const auto debtorSettle = flows.settle(debtor.id, debtorFlow.value->flowNo,
                                         debtorStart.value->version,
                                         "USER_STOPPED", now + 62 * second);
  tests.check(debtorSettle.ok() && debtorSettle.value->paidCent == 600 &&
                  debtorSettle.value->debtAddedCent ==
                      debtorSettle.value->amountCent - 600 &&
                  debtorSettle.value->debtAfterCent ==
                      debtorSettle.value->debtAddedCent,
              "settlement with a tiny balance records debt");
  const auto blockedAfterDebt = flows.createFlow(
      debtor.id, kZgcStationId, kDcFast, std::nullopt, now + 63 * second);
  tests.check(blockedAfterDebt.error ==
                  ncs::core::domain::ErrorCode::DebtOutstanding,
              "outstanding debt blocks new flows");
  tests.check(wallet.recharge(debtor.id, 1000, now + 64 * second).ok(),
              "debtor recharges a partial amount");
  const auto partialWallet = wallet.overview(debtor.id);
  tests.check(partialWallet.balanceCent == 0 &&
                  partialWallet.debtCent ==
                      debtorSettle.value->debtAddedCent - 1000,
              "partial recharge pays the oldest debt first");
  tests.check(wallet.recharge(debtor.id, 7000, now + 65 * second).ok(),
              "debtor recharges the rest");
  const auto clearedWallet = wallet.overview(debtor.id);
  tests.check(clearedWallet.balanceCent == 500 && clearedWallet.debtCent == 0,
              "full debt clearance leaves the remaining balance");
  const auto cleared = flows.createFlow(debtor.id, kZgcStationId, kDcFast,
                                        std::nullopt, now + 66 * second);
  tests.check(cleared.ok(), "cleared debt unlocks new flows");
  tests.check(flows
                  .cancel(debtor.id, cleared.value->flowNo, "USER_CANCELLED",
                          cleared.value->version, now + 67 * second)
                  .ok(),
              "queued-stage cancel keeps the account clean");

  // Queue: occupy every DC pile, queue the next user, cancel frees a slot.
  std::vector<std::string> holdingFlows;
  tests.check(wallet.recharge(broke.id, 2000, now + 70 * second).ok(),
              "broke account funded");
  tests.check(wallet.recharge(holder.id, 2000, now + 70 * second).ok(),
              "holder account funded");
  tests.check(wallet.recharge(queuer.id, 2000, now + 70 * second).ok(),
              "queuer account funded");
  for (UserAccount *accountHolder : {&driver, &broke, &holder}) {
    const auto holding =
        flows.createFlow(accountHolder->id, kZgcStationId, kDcFast,
                         std::nullopt, now + 71 * second);
    tests.check(holding.ok(), "holding flow occupies a device");
    if (holding.ok())
      holdingFlows.push_back(holding.value->flowNo);
  }
  const auto queuedFlow = flows.createFlow(queuer.id, kZgcStationId, kDcFast,
                                           std::nullopt, now + 71 * second);
  tests.check(queuedFlow.ok() && queuedFlow.value->status == 10 &&
                  queuedFlow.value->queuePosition == 1,
              "exhausted devices enqueue the request in FIFO order");
  const auto cancel = flows.cancel(driver.id, holdingFlows[0], "USER_CANCELLED",
                                   1, now + 72 * second);
  tests.check(cancel.ok() && cancel.value->status == 70,
              "pending-quote flow can be cancelled");
  const auto promoted =
      flows.flowView(queuer.id, queuedFlow.value->flowNo, now + 72 * second);
  tests.check(promoted.ok() && promoted.value->status == 20 &&
                  promoted.value->quote && promoted.value->version == 2,
              "cancellation promotes the queued flow with a fresh quote");
  tests.check(flows
                  .cancel(broke.id, holdingFlows[1], "USER_CANCELLED", 1,
                          now + 73 * second)
                  .ok(),
              "second holding flow released for the expiry tests");

  // Quote expiry: the runtime maintenance expires stale quotes and releases.
  const auto staleQuoteFlow = flows.createFlow(broke.id, kZgcStationId, kDcFast,
                                               std::nullopt, now + 80 * second);
  tests.check(staleQuoteFlow.ok() && staleQuoteFlow.value->status == 20,
              "quote created for the expiry test");
  flows.runMaintenance(now + 80 * second + std::chrono::minutes(6));
  const auto expired =
      flows.flowView(broke.id, staleQuoteFlow.value->flowNo, now + 90 * second);
  tests.check(expired.ok() && expired.value->status == 90,
              "maintenance expires a stale quote");
  const auto afterExpiry = accounts.findById(broke.id);
  tests.check(afterExpiry && !afterExpiry->hasActiveFlow,
              "expired flow clears the active-flow mirror");

  // Reservation expiry: the confirmed flow and its order expire together.
  const auto reserved = flows.createFlow(broke.id, kZgcStationId, kDcFast,
                                         std::nullopt, now + 100 * second);
  const auto reservedConfirm = flows.confirmQuote(
      broke.id, reserved.value->flowNo, reserved.value->quote->quoteNo,
      reserved.value->version, now + 101 * second);
  tests.check(reservedConfirm.ok(), "reservation created for the expiry test");
  flows.runMaintenance(now + 101 * second + std::chrono::minutes(16));
  const auto expiredReservation =
      flows.flowView(broke.id, reserved.value->flowNo, now + 120 * second);
  tests.check(expiredReservation.ok() && expiredReservation.value->status == 90,
              "maintenance expires a stale reservation");

  // Recovery: mirrors are repaired for active flows after a restart.
  const auto resumed = flows.createFlow(driver.id, kZgcStationId, kDcFast,
                                        std::nullopt, now + 130 * second);
  const auto resumedConfirm = flows.confirmQuote(
      driver.id, resumed.value->flowNo, resumed.value->quote->quoteNo,
      resumed.value->version, now + 131 * second);
  const auto resumedStart = flows.start(
      driver.id, resumed.value->flowNo, resumedConfirm.value->version,
      std::nullopt, std::nullopt, now + 132 * second);
  tests.check(resumedStart.ok(), "charging flow exists for the recovery test");
  accounts.setActiveFlowFlag(driver.id, false);
  const int recoveredCount = flows.recoverAtStartup(now + 133 * second);
  tests.check(recoveredCount >= 1, "recovery scans the active flows");
  const auto repaired = accounts.findById(driver.id);
  tests.check(repaired && repaired->hasActiveFlow,
              "recovery repairs the active-flow mirror");
  const auto resumedProgress =
      flows.progress(driver.id, resumed.value->flowNo, now + 134 * second);
  tests.check(resumedProgress.ok() && resumedProgress.value->durationSec == 120,
              "charging resumes billing from the started time");
  const auto resumedSettle = flows.settle(driver.id, resumed.value->flowNo,
                                          resumedStart.value->version,
                                          "USER_STOPPED", now + 136 * second);
  tests.check(resumedSettle.ok() && resumedSettle.value->amountCent == 540,
              "recovered flow settles normally");

  // Orders: list and receipt only expose the owner's settled order.
  const auto orders = flows.orders(driver.id, std::nullopt, 0, 0, "", 1, 20);
  bool foundSettled = false;
  for (const auto &order : orders.value->items) {
    foundSettled = foundSettled || order.orderNo == settled.value->orderNo;
  }
  tests.check(orders.ok() && orders.value->total >= 1 && foundSettled,
              "settled order appears in the user's order list");
  const auto receipt = flows.receipt(driver.id, settled.value->orderNo);
  tests.check(receipt.ok() && receipt.value->amountCent == 8100,
              "receipt replays the settlement numbers");
  const auto foreign = flows.receipt(debtor.id, settled.value->orderNo);
  tests.check(foreign.error == ncs::core::domain::ErrorCode::NotFound,
              "receipts are owner-only");

  // Capacity that appears outside the normal release path must still serve
  // the existing FIFO before a new request.
  {
    InMemoryUserAccountRepository fairAccounts;
    InMemoryChargingRepository fairRepository;
    BusinessNumbers fairNumbers;
    WalletService fairWallet(fairRepository, fairAccounts, fairNumbers);
    ChargeFlowService fairFlows(fairRepository, fairAccounts, fairAccounts,
                                fairNumbers, 60);
    UserAccount older = makeAccount("older_driver", "13800139101");
    UserAccount newcomer = makeAccount("new_driver", "13800139102");
    fairAccounts.create(older);
    fairAccounts.create(newcomer);
    fairWallet.recharge(older.id, 2000, now);
    fairWallet.recharge(newcomer.id, 2000, now);
    auto dcChargers = fairRepository.chargers(
        kZgcStationId, ChargerType::DcFast, std::nullopt);
    for (auto charger : dcChargers) {
      charger.status = ChargerStatus::Faulty;
      fairRepository.saveCharger(charger);
    }
    const auto olderFlow = fairFlows.createFlow(
        older.id, kZgcStationId, kDcFast, std::nullopt, now);
    auto restored = dcChargers.front();
    restored.status = ChargerStatus::Idle;
    fairRepository.saveCharger(restored);
    const auto newcomerFlow = fairFlows.createFlow(
        newcomer.id, kZgcStationId, kDcFast, std::nullopt, now + second);
    const auto promotedOlder = fairFlows.flowView(
        older.id, olderFlow.value->flowNo, now + second);
    tests.check(olderFlow.value->status == 10 && promotedOlder.value->status == 20 &&
                    newcomerFlow.value->status == 10,
                "newly available capacity serves the older queued flow first");
  }

  // Restart must not allocate the released charger until its two-second
  // unavailable window has completed.
  {
    InMemoryUserAccountRepository restartAccounts;
    InMemoryChargingRepository restartRepository;
    BusinessNumbers restartNumbers;
    WalletService restartWallet(restartRepository, restartAccounts,
                                restartNumbers);
    ChargeFlowService restartFlows(restartRepository, restartAccounts,
                                   restartAccounts, restartNumbers, 60);
    InMemoryAdminRepository restartAdmin(restartRepository, restartAccounts,
                                         true);
    AdminStationService stations(restartAdmin, restartRepository,
                                 restartFlows, restartNumbers);
    std::vector<UserAccount> users;
    for (int index = 0; index < 4; ++index) {
      auto user = makeAccount("restart_" + std::to_string(index),
                              "1380013920" + std::to_string(index));
      restartAccounts.create(user);
      restartWallet.recharge(user.id, 2000, now);
      users.push_back(user);
    }
    std::vector<FlowView> allocated;
    for (int index = 0; index < 3; ++index)
      allocated.push_back(*restartFlows.createFlow(
          users[index].id, kZgcStationId, kDcFast, std::nullopt, now).value);
    const auto waiting = restartFlows.createFlow(
        users[3].id, kZgcStationId, kDcFast, std::nullopt, now);
    const auto command = stations.createRestartCommand(
        1, *allocated.front().chargerId, "重启排队测试", now);
    const auto stillWaiting = restartFlows.flowView(
        users[3].id, waiting.value->flowNo, now + second);
    tests.check(command.ok() && stillWaiting.value->status == 10 &&
                    restartRepository.charger(*allocated.front().chargerId)
                            ->status == ChargerStatus::Restarting,
                "restart release cannot strand a promoted flow on restarting hardware");
    stations.completeDueCommands(now + std::chrono::seconds(3));
    const auto afterRestart = restartFlows.flowView(
        users[3].id, waiting.value->flowNo, now + std::chrono::seconds(3));
    tests.check(afterRestart.value->status == 20 &&
                    afterRestart.value->chargerId == allocated.front().chargerId,
                "restart completion promotes the FIFO head onto restored capacity");
  }
  return tests.result();
}
