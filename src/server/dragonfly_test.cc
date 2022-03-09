// Copyright 2021, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

extern "C" {
#include "redis/sds.h"
#include "redis/zmalloc.h"
}

#include <absl/strings/ascii.h>
#include <absl/strings/str_join.h>
#include <absl/strings/strip.h>
#include <gmock/gmock.h>

#include "base/gtest.h"
#include "base/logging.h"
#include "facade/facade_test.h"
#include "server/conn_context.h"
#include "server/main_service.h"
#include "server/test_utils.h"
#include "util/uring/uring_pool.h"

namespace dfly {

using namespace absl;
using namespace boost;
using namespace std;
using namespace util;
using ::io::Result;
using testing::ElementsAre;
using testing::HasSubstr;

namespace {

constexpr unsigned kPoolThreadCount = 4;

const char kKey1[] = "x";
const char kKey2[] = "b";
const char kKey3[] = "c";
const char kKey4[] = "y";

}  // namespace

// This test is responsible for server and main service
// (connection, transaction etc) families.
class DflyEngineTest : public BaseFamilyTest {
 protected:
  DflyEngineTest() : BaseFamilyTest() {
    num_threads_ = kPoolThreadCount;
  }
};

// TODO: to implement equivalent parsing in redis parser.
TEST_F(DflyEngineTest, Sds) {
  int argc;
  sds* argv = sdssplitargs("\r\n", &argc);
  EXPECT_EQ(0, argc);
  sdsfreesplitres(argv, argc);

  argv = sdssplitargs("\026 \020 \200 \277 \r\n", &argc);
  EXPECT_EQ(4, argc);
  EXPECT_STREQ("\026", argv[0]);
  sdsfreesplitres(argv, argc);

  argv = sdssplitargs(R"(abc "oops\n" )"
                      "\r\n",
                      &argc);
  EXPECT_EQ(2, argc);
  EXPECT_STREQ("oops\n", argv[1]);
  sdsfreesplitres(argv, argc);

  argv = sdssplitargs(R"( "abc\xf0" )"
                      "\t'oops\n'  \r\n",
                      &argc);
  ASSERT_EQ(2, argc);
  EXPECT_STREQ("abc\xf0", argv[0]);
  EXPECT_STREQ("oops\n", argv[1]);
  sdsfreesplitres(argv, argc);
}

TEST_F(DflyEngineTest, Multi) {
  RespVec resp = Run({"multi"});
  ASSERT_THAT(resp, RespEq("OK"));

  resp = Run({"get", kKey1});
  ASSERT_THAT(resp, RespEq("QUEUED"));

  resp = Run({"get", kKey4});
  ASSERT_THAT(resp, RespEq("QUEUED"));

  resp = Run({"exec"});
  ASSERT_THAT(resp, ElementsAre(ArgType(RespExpr::NIL), ArgType(RespExpr::NIL)));

  atomic_bool tx_empty = true;

  ess_->RunBriefInParallel([&](EngineShard* shard) {
    if (!shard->txq()->Empty())
      tx_empty.store(false);
  });
  EXPECT_TRUE(tx_empty);

  resp = Run({"get", kKey4});
  ASSERT_THAT(resp, ElementsAre(ArgType(RespExpr::NIL)));

  ASSERT_FALSE(service_->IsLocked(0, kKey1));
  ASSERT_FALSE(service_->IsLocked(0, kKey4));
  ASSERT_FALSE(service_->IsShardSetLocked());
}

TEST_F(DflyEngineTest, MultiEmpty) {
  RespVec resp = Run({"multi"});
  ASSERT_THAT(resp, RespEq("OK"));
  resp = Run({"exec"});

  ASSERT_THAT(resp[0], ArrLen(0));
  ASSERT_FALSE(service_->IsShardSetLocked());
}

TEST_F(DflyEngineTest, MultiSeq) {
  RespVec resp = Run({"multi"});
  ASSERT_THAT(resp, RespEq("OK"));

  resp = Run({"set", kKey1, absl::StrCat(1)});
  ASSERT_THAT(resp, RespEq("QUEUED"));
  resp = Run({"get", kKey1});
  ASSERT_THAT(resp, RespEq("QUEUED"));
  resp = Run({"mget", kKey1, kKey4});
  ASSERT_THAT(resp, RespEq("QUEUED"));
  resp = Run({"exec"});

  ASSERT_FALSE(service_->IsLocked(0, kKey1));
  ASSERT_FALSE(service_->IsLocked(0, kKey4));
  ASSERT_FALSE(service_->IsShardSetLocked());

  EXPECT_THAT(resp, ElementsAre(StrArg("OK"), StrArg("1"), ArrLen(2)));
  const RespExpr::Vec& arr = *get<RespVec*>(resp[2].u);
  ASSERT_THAT(arr, ElementsAre("1", ArgType(RespExpr::NIL)));
}

TEST_F(DflyEngineTest, MultiConsistent) {
  auto mset_fb = pp_->at(0)->LaunchFiber([&] {
    for (size_t i = 1; i < 10; ++i) {
      string base = StrCat(i * 900);
      RespVec resp = Run({"mset", kKey1, base, kKey4, base});
      ASSERT_THAT(resp, RespEq("OK"));
    }
  });

  auto fb = pp_->at(1)->LaunchFiber([&] {
    RespVec resp = Run({"multi"});
    ASSERT_THAT(resp, RespEq("OK"));
    this_fiber::sleep_for(1ms);

    resp = Run({"get", kKey1});
    ASSERT_THAT(resp, RespEq("QUEUED"));

    resp = Run({"get", kKey4});
    ASSERT_THAT(resp, RespEq("QUEUED"));

    resp = Run({"mget", kKey4, kKey1});
    ASSERT_THAT(resp, RespEq("QUEUED"));

    resp = Run({"exec"});

    EXPECT_THAT(resp, ElementsAre(ArgType(RespExpr::STRING), ArgType(RespExpr::STRING),
                                  ArgType(RespExpr::ARRAY)));
    ASSERT_EQ(resp[0].GetBuf(), resp[1].GetBuf());
    const RespVec& arr = *get<RespVec*>(resp[2].u);
    EXPECT_THAT(arr, ElementsAre(ArgType(RespExpr::STRING), ArgType(RespExpr::STRING)));
    EXPECT_EQ(arr[0].GetBuf(), arr[1].GetBuf());
    EXPECT_EQ(arr[0].GetBuf(), resp[0].GetBuf());
  });

  mset_fb.join();
  fb.join();
  ASSERT_FALSE(service_->IsLocked(0, kKey1));
  ASSERT_FALSE(service_->IsLocked(0, kKey4));
  ASSERT_FALSE(service_->IsShardSetLocked());
}

TEST_F(DflyEngineTest, MultiRename) {
  RespVec resp = Run({"multi"});
  ASSERT_THAT(resp, RespEq("OK"));
  Run({"set", kKey1, "1"});

  resp = Run({"rename", kKey1, kKey4});
  ASSERT_THAT(resp, RespEq("QUEUED"));
  resp = Run({"exec"});

  EXPECT_THAT(resp, ElementsAre(StrArg("OK"), StrArg("OK")));
  ASSERT_FALSE(service_->IsLocked(0, kKey1));
  ASSERT_FALSE(service_->IsLocked(0, kKey4));
  ASSERT_FALSE(service_->IsShardSetLocked());
}

TEST_F(DflyEngineTest, MultiHop) {
  Run({"set", kKey1, "1"});

  auto p1_fb = pp_->at(1)->LaunchFiber([&] {
    for (int i = 0; i < 100; ++i) {
      auto resp = Run({"rename", kKey1, kKey2});
      ASSERT_THAT(resp, RespEq("OK"));
      EXPECT_EQ(2, GetDebugInfo("IO1").shards_count);

      resp = Run({"rename", kKey2, kKey1});
      ASSERT_THAT(resp, RespEq("OK"));
    }
  });

  // mset should be executed either as ooo or via tx-queue because previous transactions
  // have been unblocked and executed as well. In other words, this mset should never block
  // on serializability constraints.
  auto p2_fb = pp_->at(2)->LaunchFiber([&] {
    for (int i = 0; i < 100; ++i) {
      Run({"mset", kKey3, "1", kKey4, "2"});
    }
  });

  p1_fb.join();
  p2_fb.join();
}

TEST_F(DflyEngineTest, FlushDb) {
  Run({"mset", kKey1, "1", kKey4, "2"});
  auto resp = Run({"flushdb"});
  ASSERT_THAT(resp, RespEq("OK"));

  auto fb0 = pp_->at(0)->LaunchFiber([&] {
    for (unsigned i = 0; i < 100; ++i) {
      Run({"flushdb"});
    }
  });

  pp_->at(1)->Await([&] {
    for (unsigned i = 0; i < 100; ++i) {
      Run({"mset", kKey1, "1", kKey4, "2"});
      auto resp = Run({"exists", kKey1, kKey4});
      int64_t ival = get<int64_t>(resp[0].u);
      ASSERT_TRUE(ival == 0 || ival == 2) << i << " " << ival;
    }
  });

  fb0.join();

  ASSERT_FALSE(service_->IsLocked(0, kKey1));
  ASSERT_FALSE(service_->IsLocked(0, kKey4));
  ASSERT_FALSE(service_->IsShardSetLocked());
}

TEST_F(DflyEngineTest, Eval) {
  auto resp = Run({"eval", "return 43", "0"});
  EXPECT_THAT(resp[0], IntArg(43));

  resp = Run({"incrby", "foo", "42"});
  EXPECT_THAT(resp[0], IntArg(42));

  resp = Run({"eval", "return redis.call('get', 'foo')", "0"});
  EXPECT_THAT(resp[0], ErrArg("undeclared"));

  resp = Run({"eval", "return redis.call('get', 'foo')", "1", "bar"});
  EXPECT_THAT(resp[0], ErrArg("undeclared"));

  ASSERT_FALSE(service_->IsLocked(0, "foo"));

  resp = Run({"eval", "return redis.call('get', 'foo')", "1", "foo"});
  EXPECT_THAT(resp[0], StrArg("42"));

  resp = Run({"eval", "return redis.call('get', KEYS[1])", "1", "foo"});
  EXPECT_THAT(resp[0], StrArg("42"));

  ASSERT_FALSE(service_->IsLocked(0, "foo"));
  ASSERT_FALSE(service_->IsShardSetLocked());
}

TEST_F(DflyEngineTest, EvalSha) {
  auto resp = Run({"script", "load", "return 5"});
  EXPECT_THAT(resp, ElementsAre(ArgType(RespExpr::STRING)));

  string sha{ToSV(resp[0].GetBuf())};

  resp = Run({"evalsha", sha, "0"});
  EXPECT_THAT(resp[0], IntArg(5));

  resp = Run({"script", "load", " return 5  "});
  EXPECT_THAT(resp, ElementsAre(StrArg(sha)));

  absl::AsciiStrToUpper(&sha);
  resp = Run({"evalsha", sha, "0"});
  EXPECT_THAT(resp[0], IntArg(5));

  resp = Run({"evalsha", "foobar", "0"});
  EXPECT_THAT(resp[0], ErrArg("No matching"));
}

TEST_F(DflyEngineTest, Memcache) {
  using MP = MemcacheParser;

  auto resp = RunMC(MP::SET, "key", "bar", 1);
  EXPECT_THAT(resp, ElementsAre("STORED"));

  resp = RunMC(MP::GET, "key");
  EXPECT_THAT(resp, ElementsAre("VALUE key 1 3", "bar", "END"));

  resp = RunMC(MP::ADD, "key", "bar", 1);
  EXPECT_THAT(resp, ElementsAre("NOT_STORED"));

  resp = RunMC(MP::REPLACE, "key2", "bar", 1);
  EXPECT_THAT(resp, ElementsAre("NOT_STORED"));

  resp = RunMC(MP::ADD, "key2", "bar2", 2);
  EXPECT_THAT(resp, ElementsAre("STORED"));

  resp = GetMC(MP::GET, {"key2", "key"});
  EXPECT_THAT(resp, ElementsAre("VALUE key2 2 4", "bar2", "VALUE key 1 3", "bar", "END"));

  resp = RunMC(MP::APPEND, "key2", "val2", 0);
  EXPECT_THAT(resp, ElementsAre("STORED"));
  resp = RunMC(MP::GET, "key2");
  EXPECT_THAT(resp, ElementsAre("VALUE key2 2 8", "bar2val2", "END"));

  resp = RunMC(MP::APPEND, "unkn", "val2", 0);
  EXPECT_THAT(resp, ElementsAre("NOT_STORED"));

  resp = RunMC(MP::GET, "unkn");
  EXPECT_THAT(resp, ElementsAre("END"));
}

// TODO: to test transactions with a single shard since then all transactions become local.
// To consider having a parameter in dragonfly engine controlling number of shards
// unconditionally from number of cpus. TO TEST BLPOP under multi for single/multi argument case.

}  // namespace dfly