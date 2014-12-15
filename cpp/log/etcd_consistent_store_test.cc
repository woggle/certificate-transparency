#include "log/etcd_consistent_store-inl.h"

#include <atomic>
#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "log/logged_certificate.h"
#include "proto/ct.pb.h"
#include "util/fake_etcd.h"
#include "util/libevent_wrapper.h"
#include "util/testing.h"
#include "util/util.h"

namespace cert_trans {


using std::atomic;
using std::bind;
using std::make_shared;
using std::pair;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_ptr;
using std::vector;
using testing::_;
using testing::AllOf;
using testing::Contains;
using testing::Pair;
using testing::Return;
using testing::SetArgumentPointee;
using util::Status;


const char kRoot[] = "/root";
const char kNodeId[] = "node_id";
const int kTimestamp = 9000;


void DoNothing() {
}


class EtcdConsistentStoreTest : public ::testing::Test {
 public:
  EtcdConsistentStoreTest()
      : base_(make_shared<libevent::Base>()),
        client_(base_),
        sync_client_(&client_),
        running_(true) {
    event_pump_.reset(
        new thread(bind(&EtcdConsistentStoreTest::EventPump, this)));
  }

  ~EtcdConsistentStoreTest() {
    running_.store(false);
    base_->Add(bind(&DoNothing));
    event_pump_->join();
  }

  void EventPump() {
    libevent::Event event(*base_, -1, 0, std::bind(&DoNothing));
    event.Add(std::chrono::seconds(60));
    while (running_) {
      base_->DispatchOnce();
    }
  }

 protected:
  void SetUp() override {
    store_.reset(
        new EtcdConsistentStore<LoggedCertificate>(&client_, kRoot, kNodeId));
  }

  LoggedCertificate DefaultCert() {
    return MakeCert(kTimestamp, "leaf");
  }

  LoggedCertificate MakeCert(int timestamp, const string& body) {
    LoggedCertificate cert;
    cert.mutable_sct()->set_timestamp(timestamp);
    cert.mutable_entry()->set_type(ct::X509_ENTRY);
    cert.mutable_entry()->mutable_x509_entry()->set_leaf_certificate(body);
    return cert;
  }

  LoggedCertificate MakeSequencedCert(int timestamp, const string& body,
                                      int seq) {
    LoggedCertificate cert(MakeCert(timestamp, body));
    cert.set_sequence_number(seq);
    return cert;
  }

  EntryHandle<LoggedCertificate> HandleForCert(const LoggedCertificate& cert) {
    return EntryHandle<LoggedCertificate>(cert);
  }

  EntryHandle<LoggedCertificate> HandleForCert(const LoggedCertificate& cert,
                                               int handle) {
    return EntryHandle<LoggedCertificate>(cert, handle);
  }

  template <class T>
  void InsertEntry(const string& key, const T& thing) {
    // Set up scenario:
    int64_t index;
    Status status(sync_client_.Create(key, Serialize(thing), &index));
    ASSERT_TRUE(status.ok()) << status;
  }

  template <class T>
  void PeekEntry(const string& key, T* thing) {
    EtcdClient::Node node;
    Status status(sync_client_.Get(key, &node));
    ASSERT_TRUE(status.ok()) << status;
    Deserialize(node.value_, thing);
  }

  template <class T>
  string Serialize(const T& t) {
    string flat;
    t.SerializeToString(&flat);
    return util::ToBase64(flat);
  }

  template <class T>
  void Deserialize(const string& flat, T* t) {
    ASSERT_TRUE(t->ParseFromString(util::FromBase64(flat.c_str())));
  }

  template <class T>
  EtcdClient::Node NodeFor(const int index, const std::string& key,
                           const T& t) {
    return EtcdClient::Node(index, index, key, Serialize(t));
  }

  shared_ptr<libevent::Base> base_;
  FakeEtcdClient client_;
  SyncEtcdClient sync_client_;
  atomic<bool> running_;
  unique_ptr<thread> event_pump_;
  unique_ptr<EtcdConsistentStore<LoggedCertificate>> store_;
};


typedef class EtcdConsistentStoreTest EtcdConsistentStoreDeathTest;

TEST_F(EtcdConsistentStoreDeathTest, TestNextAvailableSequenceNumber) {
  EXPECT_DEATH(store_->NextAvailableSequenceNumber(), "Not Implemented");
}


TEST_F(EtcdConsistentStoreTest, TestSetServingSTH) {
  ct::SignedTreeHead sth;
  EXPECT_EQ(util::error::UNIMPLEMENTED,
            store_->SetServingSTH(sth).CanonicalCode());
}


TEST_F(EtcdConsistentStoreTest, TestAddPendingEntryWorks) {
  LoggedCertificate cert(DefaultCert());
  util::Status status(store_->AddPendingEntry(&cert));
  ASSERT_TRUE(status.ok()) << status;
  EtcdClient::Node node;
  status = sync_client_.Get(string(kRoot) + "/unsequenced/" +
                                util::ToBase64(cert.Hash()),
                            &node);
  EXPECT_TRUE(status.ok()) << status;
  EXPECT_EQ(Serialize(cert), node.value_);
}


TEST_F(EtcdConsistentStoreTest,
       TestAddPendingEntryForExistingEntryReturnsSct) {
  LoggedCertificate cert(DefaultCert());
  LoggedCertificate other_cert(DefaultCert());
  other_cert.mutable_sct()->set_timestamp(55555);

  const string kKey(util::ToBase64(cert.Hash()));
  const string kPath(string(kRoot) + "/unsequenced/" + kKey);
  // Set up scenario:
  InsertEntry(kPath, other_cert);

  util::Status status(store_->AddPendingEntry(&cert));
  EXPECT_EQ(util::error::ALREADY_EXISTS, status.CanonicalCode());
  EXPECT_EQ(other_cert.timestamp(), cert.timestamp());
}


TEST_F(EtcdConsistentStoreDeathTest,
       TestAddPendingEntryForExistingNonIdenticalEntry) {
  LoggedCertificate cert(DefaultCert());
  LoggedCertificate other_cert(MakeCert(2342, "something else"));

  const string kKey(util::ToBase64(cert.Hash()));
  const string kPath(string(kRoot) + "/unsequenced/" + kKey);
  // Set up scenario:
  InsertEntry(kPath, other_cert);

  EXPECT_DEATH(store_->AddPendingEntry(&cert),
               "preexisting_entry.*==.*entry.*");
}


TEST_F(EtcdConsistentStoreDeathTest,
       TestAddPendingEntryDoesNotAcceptSequencedEntry) {
  LoggedCertificate cert(DefaultCert());
  cert.set_sequence_number(76);
  EXPECT_DEATH(store_->AddPendingEntry(&cert),
               "!entry\\->has_sequence_number");
}


TEST_F(EtcdConsistentStoreTest, TestGetPendingEntryForHash) {
  const LoggedCertificate one(MakeCert(123, "one"));
  const string kPath(string(kRoot) + "/unsequenced/" +
                     util::ToBase64(one.Hash()));
  InsertEntry(kPath, one);

  EntryHandle<LoggedCertificate> handle;
  util::Status status(store_->GetPendingEntryForHash(one.Hash(), &handle));
  EXPECT_TRUE(status.ok()) << status;
  EXPECT_EQ(one, handle.Entry());
  EXPECT_EQ(1, handle.Handle());
}


TEST_F(EtcdConsistentStoreTest, TestGetPendingEntryForNonExistantHash) {
  const string kPath(string(kRoot) + "/unsequenced/" + util::ToBase64("Nah"));
  EntryHandle<LoggedCertificate> handle;
  util::Status status(store_->GetPendingEntryForHash("Nah", &handle));
  EXPECT_EQ(util::error::NOT_FOUND, status.CanonicalCode()) << status;
}


TEST_F(EtcdConsistentStoreTest, TestGetPendingEntries) {
  const string kPath(string(kRoot) + "/unsequenced/");
  const LoggedCertificate one(MakeCert(123, "one"));
  const LoggedCertificate two(MakeCert(456, "two"));
  InsertEntry(kPath + "one", one);
  InsertEntry(kPath + "two", two);

  vector<EntryHandle<LoggedCertificate>> entries;
  util::Status status(store_->GetPendingEntries(&entries));
  EXPECT_TRUE(status.ok()) << status;
  EXPECT_EQ(2, entries.size());
  vector<LoggedCertificate> certs;
  for (const auto& e : entries) {
    certs.push_back(e.Entry());
  }
  EXPECT_THAT(certs, AllOf(Contains(one), Contains(two)));
}


TEST_F(EtcdConsistentStoreDeathTest,
       TestGetPendingEntriesBarfsWithSequencedEntry) {
  const string kPath(string(kRoot) + "/unsequenced/");
  LoggedCertificate one(MakeSequencedCert(123, "one", 666));
  InsertEntry(kPath + "one", one);
  vector<EntryHandle<LoggedCertificate>> entries;
  EXPECT_DEATH(store_->GetPendingEntries(&entries), "has_sequence_number");
}


TEST_F(EtcdConsistentStoreTest, TestGetSequencedEntries) {
  const string kPath(string(kRoot) + "/sequenced/");
  const LoggedCertificate one(MakeSequencedCert(123, "one", 1));
  const LoggedCertificate two(MakeSequencedCert(456, "two", 2));
  InsertEntry(kPath + "one", one);
  InsertEntry(kPath + "two", two);
  vector<EntryHandle<LoggedCertificate>> entries;
  util::Status status(store_->GetSequencedEntries(&entries));
  EXPECT_EQ(2, entries.size());
  vector<LoggedCertificate> certs;
  for (const auto& e : entries) {
    certs.push_back(e.Entry());
  }
  EXPECT_THAT(certs, AllOf(Contains(one), Contains(two)));
}


TEST_F(EtcdConsistentStoreDeathTest,
       TestGetSequencedEntriesBarfsWitUnsSequencedEntry) {
  const string kPath(string(kRoot) + "/sequenced/");
  LoggedCertificate one(MakeCert(123, "one"));
  InsertEntry(kPath + "one", one);
  vector<EntryHandle<LoggedCertificate>> entries;
  EXPECT_DEATH(store_->GetSequencedEntries(&entries), "has_sequence_number");
}


TEST_F(EtcdConsistentStoreTest, TestAssignSequenceNumber) {
  const int kDefaultHandle(1);
  EntryHandle<LoggedCertificate> entry(
      HandleForCert(DefaultCert(), kDefaultHandle));

  const string kUnsequencedPath(string(kRoot) + "/unsequenced/" +
                                util::ToBase64(entry.Entry().Hash()));
  const string kSequencedPath(string(kRoot) + "/sequenced/1");
  const int kSeq(1);


  LoggedCertificate entry_with_provisional(entry.Entry());
  entry_with_provisional.set_provisional_sequence_number(kSeq);
  InsertEntry(kUnsequencedPath, entry_with_provisional);

  util::Status status(store_->AssignSequenceNumber(kSeq, &entry));
  EXPECT_TRUE(status.ok()) << status;
}


TEST_F(EtcdConsistentStoreDeathTest,
       TestAssignSequenceNumberBarfsWithSequencedEntry) {
  EntryHandle<LoggedCertificate> entry(
      HandleForCert(MakeSequencedCert(123, "hi", 44)));
  EXPECT_DEATH(util::Status status(store_->AssignSequenceNumber(1, &entry));
               , "has_sequence_number");
}


TEST_F(EtcdConsistentStoreDeathTest,
       TestAssignSequenceNumberBarfsWithMismatchedSequencedEntry) {
  EntryHandle<LoggedCertificate> entry(HandleForCert(MakeCert(123, "hi")));
  entry.MutableEntry()->set_provisional_sequence_number(257);
  EXPECT_DEATH(util::Status status(store_->AssignSequenceNumber(1, &entry));
               , "sequence_number ==.*provisional.*");
}


TEST_F(EtcdConsistentStoreTest, TestSetClusterNodeState) {
  const string kPath(string(kRoot) + "/nodes/" + kNodeId);

  ct::ClusterNodeState state;
  state.set_node_id(kNodeId);
  state.set_contiguous_tree_size(2342);

  util::Status status(store_->SetClusterNodeState(state));
  EXPECT_TRUE(status.ok()) << status;

  ct::ClusterNodeState set_state;
  PeekEntry(kPath, &set_state);
  EXPECT_EQ(state.node_id(), set_state.node_id());
  EXPECT_EQ(state.contiguous_tree_size(), set_state.contiguous_tree_size());
}


}  // namespace cert_trans

int main(int argc, char** argv) {
  cert_trans::test::InitTesting(argv[0], &argc, &argv, true);
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  return RUN_ALL_TESTS();
}