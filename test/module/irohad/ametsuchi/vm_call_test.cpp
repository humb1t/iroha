/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ametsuchi/vmCall.h"

#include <gtest/gtest.h>
#include <boost/mpl/back_inserter.hpp>
#include <boost/mpl/copy.hpp>
#include <boost/mpl/count.hpp>
#include <boost/mpl/find.hpp>
#include <unordered_set>
#include "backend/protobuf/proto_query_response_factory.hpp"
#include "interfaces/commands/add_asset_quantity.hpp"
#include "interfaces/commands/add_peer.hpp"
#include "interfaces/commands/add_signatory.hpp"
#include "interfaces/commands/append_role.hpp"
#include "interfaces/commands/command.hpp"
#include "interfaces/commands/compare_and_set_account_detail.hpp"
#include "interfaces/commands/create_account.hpp"
#include "interfaces/commands/create_asset.hpp"
#include "interfaces/commands/create_domain.hpp"
#include "interfaces/commands/create_role.hpp"
#include "interfaces/commands/detach_role.hpp"
#include "interfaces/commands/engine_call.hpp"
#include "interfaces/commands/grant_permission.hpp"
#include "interfaces/commands/remove_peer.hpp"
#include "interfaces/commands/remove_signatory.hpp"
#include "interfaces/commands/revoke_permission.hpp"
#include "interfaces/commands/set_account_detail.hpp"
#include "interfaces/commands/set_quorum.hpp"
#include "interfaces/commands/subtract_asset_quantity.hpp"
#include "interfaces/commands/transfer_asset.hpp"
#include "interfaces/queries/blocks_query.hpp"
#include "interfaces/queries/get_account.hpp"
#include "interfaces/queries/query.hpp"
#include "module/irohad/ametsuchi/mock_command_executor.hpp"
#include "module/irohad/ametsuchi/mock_query_executor_visitor.hpp"

template <typename T>
class VariantTypeMatcher {
 public:
  template <typename Variant>
  bool MatchAndExplain(const Variant &value,
                       ::testing::MatchResultListener *listener) const {
    using VariantTypes = typename std::decay_t<decltype(value.get())>::types;
    using VariantTypesVector = typename boost::mpl::copy<
        VariantTypes,
        boost::mpl::back_inserter<boost::mpl::vector<>>>::type;
    static_assert(
        boost::mpl::count<VariantTypesVector, T>::type::value == 1,
        "The type must occur exactly once in the list of variant alternatives");
    return value.get().which()
        == boost::mpl::find<VariantTypesVector, T>::type::pos::value;
  }

  virtual void DescribeTo(::std::ostream *os) const {
    *os << "Tested variant contains expected type.";
  }

  virtual void DescribeNegationTo(::std::ostream *os) const {
    *os << "Tested variant does not contain expected type.";
  }
};

template <typename T>
inline auto VariantWithType() {
  return ::testing::MakePolymorphicMatcher(VariantTypeMatcher<T>());
}

TEST(VmCallTest, UsageTest) {
  /*

code is bytecode from the following Solidity code using online Remix IDE with
compiler version 0.4.0

pragma solidity ^0.4.0;

contract C {
  uint256 a;
  function setA(uint256 _a) {
    a = _a;
  }

  function getA() returns(uint256) {
    return a;
  }
}

*/

  char *code = const_cast<char *>(
      "606060405260a18060106000396000f360606040526000357c0100000000000000000000"
      "00000000000000000000000000000000000090048063d46300fd146043578063ee919d50"
      "14606857603f565b6002565b34600257605260048050506082565b604051808281526020"
      "0191505060405180910390f35b3460025760806004808035906020019091905050609356"
      "5b005b600060006000505490506090565b90565b806000600050819055505b5056");

  /*
    calling setA(uint256), bytes4(keccak256(setA(uint256))) == ee919d50, and
    append uint256 equal to 1 as the parameter
  */

  char *inputCallSetter = const_cast<char *>(
      "ee919d50000000000000000000000000000000000000000000000000000000000000000"
      "1");

  /*
    calling getA(), bytes4(keccak256(getA())) == d46300fd
  */

  char *inputCallGetter = const_cast<char *>("d46300fd");

  char *caller = const_cast<char *>("caller"),
       *callee = const_cast<char *>("Callee"), *empty = const_cast<char *>("");

  // Emulate account existence for the smart contract engine
  std::unordered_set<std::string> existingTestAccounts;

  iroha::ametsuchi::MockCommandExecutor command_executor;
  EXPECT_CALL(
      command_executor,
      execute(VariantWithType<const shared_model::interface::CreateAccount &>(),
              ::testing::_,
              ::testing::_))
      .WillRepeatedly(
          [&existingTestAccounts](const auto &cmd, const auto &, auto) {
            const auto &cmdNewAcc =
                boost::get<const shared_model::interface::CreateAccount &>(
                    cmd.get());
            existingTestAccounts.insert(cmdNewAcc.accountName());
            return iroha::ametsuchi::CommandResult{};
          });

  iroha::ametsuchi::MockSpecificQueryExecutor specific_query_executor;
  auto query_response_factory =
      std::make_shared<shared_model::proto::ProtoQueryResponseFactory>();

  EXPECT_CALL(specific_query_executor, execute(::testing::_))
      .WillRepeatedly([query_response_factory, &existingTestAccounts](
                          const shared_model::interface::Query &query) {
        const auto concreteCommand = query.get();
        const auto &queryVariant =
            static_cast<shared_model::interface::Query::QueryVariantType>(
                concreteCommand);
        const auto &getAccQuery =
            boost::get<const shared_model::interface::GetAccount &>(
                queryVariant);
        const auto &id = getAccQuery.accountId();
        if (existingTestAccounts.find(id) != existingTestAccounts.end()) {
          return query_response_factory->createAccountResponse(
              id, "@evm", 1, {}, {"user"}, {});
        } else {
          // TODO(IvanTyulyandin): Fix magic number 5
          return query_response_factory->createErrorQueryResponse(
              shared_model::interface::QueryResponseFactory::ErrorQueryType::
                  kNoAccount,
              "No such account",
              5,
              {});
        }
      });

  auto res = VmCall(
      code, empty, caller, callee, &command_executor, &specific_query_executor);
  std::cout << "Vm output: " << res.r0 << std::endl;
  ASSERT_TRUE(res.r1);

  res = VmCall(empty,
               inputCallSetter,
               caller,
               callee,
               &command_executor,
               &specific_query_executor);
  std::cout << "Vm output: " << res.r0 << std::endl;
  ASSERT_TRUE(res.r1);

  res = VmCall(empty,
               inputCallGetter,
               caller,
               callee,
               &command_executor,
               &specific_query_executor);
  std::cout << "Vm output: " << res.r0 << std::endl;
  ASSERT_TRUE(res.r1);
}