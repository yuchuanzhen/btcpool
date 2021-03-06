/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#ifndef STRATUM_SERVER_H_
#define STRATUM_SERVER_H_

#include "Common.h"

#include <map>
#include <vector>
#include <memory>
#include <bitset>

#include <arpa/inet.h>
#include <sys/socket.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>

#include <glog/logging.h>

#include "bitcoin/core.h"
#include "Kafka.h"
#include "MySQLConnection.h"
#include "Stratum.h"
#include "StratumSession.h"

class Server;
class StratumJobEx;


//////////////////////////////// SessionIDManager //////////////////////////////
// DO NOT CHANGE
#define MAX_SESSION_INDEX_SERVER   0x00FFFFFFu   // 16777215 = 2^24 - 1

// thread-safe
class SessionIDManager {
  //
  //  SESSION ID: UINT32_T
  //
  //   xxxxxxxx     xxxxxxxx xxxxxxxx xxxxxxxx
  //  ----------    --------------------------
  //  server ID          session id
  //   [1, 255]        range: [0, MAX_SESSION_INDEX_SERVER]
  //
  uint8_t serverId_;
  std::bitset<MAX_SESSION_INDEX_SERVER + 1> sessionIds_;
  uint32_t allocIdx_;
  mutex lock_;

public:
  SessionIDManager(const uint8_t serverId) : serverId_(serverId), allocIdx_(0) {
    sessionIds_.reset();
  }

  uint32_t allocSessionId() {
    ScopeLock sl(lock_);

    // find an empty bit
    while (sessionIds_.test(allocIdx_) == true) {
      allocIdx_++;
      if (allocIdx_ > MAX_SESSION_INDEX_SERVER) {
        allocIdx_ = 0;
      }
    }
    // set to true
    sessionIds_.set(allocIdx_, true);
    return ((uint32_t)serverId_ << 24) | allocIdx_;
  }

  void freeSessionId(uint32_t sessionId) {
    ScopeLock sl(lock_);
    uint32_t idx = (sessionId & 0x00FFFFFFu);
    sessionIds_.set(idx, false);
  }
};



////////////////////////////////// JobRepository ///////////////////////////////
class JobRepository {
  atomic<bool> running_;
  mutex lock_;
  std::map<uint64_t/* jobId */, shared_ptr<StratumJobEx> > exJobs_;

  KafkaConsumer kafkaConsumer_;  // consume topic: 'StratumJob'
  Server *server_;               // call server to send new job

  const time_t kMaxJobsLifeTime_;
  const time_t kMiningNotifyInterval_;

  time_t lastJobSendTime_;
  uint256 latestPrevBlockHash_;

  thread threadConsume_;
  void runThreadConsume();

  void consumeStratumJob(rd_kafka_message_t *rkmessage);
  void sendMiningNotify(shared_ptr<StratumJobEx> exJob);
  void tryCleanExpiredJobs();
  void checkAndSendMiningNotify();

public:
  JobRepository(const char *kafkaBrokers, Server *server);
  ~JobRepository();

  void stop();
  bool setupThreadConsume();
  void markAllJobsAsStale();

  shared_ptr<StratumJobEx> getStratumJobEx(const uint64_t jobId);
  shared_ptr<StratumJobEx> getLatestStratumJobEx();
};


///////////////////////////////////// UserInfo /////////////////////////////////
// 1. update userName->userId by interval
// 2. insert worker name to db
class UserInfo {
  struct WorkerName {
    int32_t userId_;
    int64_t workerId_;
    char    workerName_[21];
    char    minerAgent_[31];

    WorkerName(): userId_(0), workerId_(0) {
      memset(workerName_, 0, sizeof(workerName_));
      memset(minerAgent_, 0, sizeof(minerAgent_));
    }
  };

  //--------------------
  pthread_rwlock_t rwlock_;
  atomic<bool> running_;
  string apiUrl_;

  // username -> userId
  std::unordered_map<string, int32_t> nameIds_;
  int32_t lastMaxUserId_;

  // workerName
  mutex workerNameLock_;
  std::deque<WorkerName> workerNameQ_;

  MySQLConnection db_;
  thread threadInsertWorkerName_;
  void runThreadInsertWorkerName();
  int32_t insertWorkerName();

  thread threadUpdate_;
  void runThreadUpdate();
  int32_t updateUsers();

public:
  UserInfo(const string &apiUrl, const MysqlConnectInfo &dbInfo);
  ~UserInfo();

  void stop();
  bool setupThreads();

  int32_t getUserId(const string userName);
  void addWorker(const int32_t userId, const int64_t workerId,
                 const string &workerName, const string &minerAgent);
};


////////////////////////////////// StratumJobEx ////////////////////////////////
class StratumJobEx {
  enum State {
    MINING = 0,
    STALE  = 1
  };

  //------------
  mutex lock_;
  State state_;

  void makeMiningNotifyStr();
  void generateCoinbaseTx(std::vector<char> *coinbaseBin,
                          const uint32 extraNonce1,
                          const string &extraNonce2Hex);

public:
  bool isClean_;
  StratumJob *sjob_;
  string miningNotify1_;
  string miningNotify2_;

public:
  StratumJobEx(StratumJob *sjob, bool isClean);
  ~StratumJobEx();

  void markStale();
  bool isStale();

  void generateBlockHeader(CBlockHeader *header,
                           std::vector<char> *coinbaseBin,
                           const uint32 extraNonce1,
                           const string &extraNonce2Hex,
                           const vector<uint256> &merkleBranch,
                           const uint256 &hashPrevBlock,
                           const uint32 nBits, const int nVersion,
                           const uint32 nTime, const uint32 nonce);
};


///////////////////////////////////// Server ///////////////////////////////////
class Server {
  // NetIO
  struct sockaddr_in sin_;
  struct event_base* base_;
  struct event* signal_event_;
  struct evconnlistener* listener_;
  std::unordered_map<evutil_socket_t, StratumSession *> connections_;
  mutex connsLock_;  // lock for connections

  // Stratum
  mutex producerShareLogLock_;
  KafkaProducer *kafkaProducerShareLog_;
  mutex producerSolvedShareLock_;
  KafkaProducer *kafkaProducerSolvedShare_;

  //
  // WARNING: if enable simulator, all share will be accepted. only for test.
  //
  bool isEnableSimulator_;

public:
  const int32_t kShareAvgSeconds_;
  JobRepository *jobRepository_;
  UserInfo *userInfo_;
  SessionIDManager *sessionIDManager_;

public:
  Server();
  ~Server();

  bool setup(const char *ip, const unsigned short port, const char *kafkaBrokers,
             const string &userAPIUrl, const  MysqlConnectInfo &dbInfo,
             const uint8_t serverId, bool isEnableSimulator);
  void run();
  void stop();

  void sendMiningNotifyToAll(shared_ptr<StratumJobEx> exJobPtr);

  void addConnection   (evutil_socket_t fd, StratumSession *connection);
  void removeConnection(evutil_socket_t fd);

  static void listenerCallback(struct evconnlistener* listener,
                               evutil_socket_t socket,
                               struct sockaddr* saddr,
                               int socklen, void* server);
  static void readCallback (struct bufferevent *, void *connection);
  static void eventCallback(struct bufferevent *, short, void *connection);

  int checkShare(const Share &share,
                 const uint32 extraNonce1, const string &extraNonce2Hex,
                 const uint32_t nTime, const uint32_t nonce,
                 const uint256 &jobTarget, const string &workFullName);
  void sendShare2Kafka      (const uint8_t *data, size_t len);
  void sendSolvedShare2Kafka(const FoundBlock *foundBlock,
                             const std::vector<char> &coinbaseBin);
};


////////////////////////////////// StratumServer ///////////////////////////////
class StratumServer {
  atomic<bool> running_;

  Server server_;
  string ip_;
  unsigned short port_;
  uint8_t serverId_;  // global unique, range: [1, 255]

  string kafkaBrokers_;
  string userAPIUrl_;
  MysqlConnectInfo poolDBInfo_;

  // if enable simulator, all share will be accepted
  bool isEnableSimulator_;

public:
  StratumServer(const char *ip, const unsigned short port,
                const char *kafkaBrokers,
                const string &userAPIUrl, const MysqlConnectInfo &poolDBInfo,
                const uint8_t serverId, bool isEnableSimulator);
  ~StratumServer();

  bool init();
  void stop();
  void run();
};


#endif
