/*
 * libjingle
 * Copyright 2012, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#if defined(POSIX)
#include <dirent.h>
#endif

#include "talk/base/asynctcpsocket.h"
#include "talk/base/buffer.h"
#include "talk/base/dscp.h"
#include "talk/base/firewallsocketserver.h"
#include "talk/base/logging.h"
#include "talk/base/gunit.h"
#include "talk/base/helpers.h"
#include "talk/base/physicalsocketserver.h"
#include "talk/base/scoped_ptr.h"
#include "talk/base/socketaddress.h"
#include "talk/base/thread.h"
#include "talk/base/virtualsocketserver.h"
#include "talk/p2p/base/basicpacketsocketfactory.h"
#include "talk/p2p/base/constants.h"
#include "talk/p2p/base/tcpport.h"
#include "talk/p2p/base/testturnserver.h"
#include "talk/p2p/base/turnport.h"
#include "talk/p2p/base/udpport.h"

using talk_base::SocketAddress;
using cricket::Connection;
using cricket::Port;
using cricket::PortInterface;
using cricket::TurnPort;
using cricket::UDPPort;

static const SocketAddress kLocalAddr1("11.11.11.11", 0);
static const SocketAddress kLocalAddr2("22.22.22.22", 0);
static const SocketAddress kLocalIPv6Addr(
    "2401:fa00:4:1000:be30:5bff:fee5:c3", 0);
static const SocketAddress kTurnUdpIntAddr("99.99.99.3",
                                           cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnTcpIntAddr("99.99.99.4",
                                           cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnUdpExtAddr("99.99.99.5", 0);
static const SocketAddress kTurnUdpIPv6IntAddr(
    "2400:4030:1:2c00:be30:abcd:efab:cdef", cricket::TURN_SERVER_PORT);
static const SocketAddress kTurnUdpIPv6ExtAddr(
  "2620:0:1000:1b03:2e41:38ff:fea6:f2a4", 0);

static const char kIceUfrag1[] = "TESTICEUFRAG0001";
static const char kIceUfrag2[] = "TESTICEUFRAG0002";
static const char kIcePwd1[] = "TESTICEPWD00000000000001";
static const char kIcePwd2[] = "TESTICEPWD00000000000002";
static const char kTurnUsername[] = "test";
static const char kTurnPassword[] = "test";
static const unsigned int kTimeout = 1000;

static const cricket::ProtocolAddress kTurnUdpProtoAddr(
    kTurnUdpIntAddr, cricket::PROTO_UDP);
static const cricket::ProtocolAddress kTurnTcpProtoAddr(
    kTurnTcpIntAddr, cricket::PROTO_TCP);
static const cricket::ProtocolAddress kTurnUdpIPv6ProtoAddr(
    kTurnUdpIPv6IntAddr, cricket::PROTO_UDP);

static const unsigned int MSG_TESTFINISH = 0;

#if defined(LINUX)
static int GetFDCount() {
  struct dirent *dp;
  int fd_count = 0;
  DIR *dir = opendir("/proc/self/fd/");
  while ((dp = readdir(dir)) != NULL) {
    if (dp->d_name[0] == '.')
      continue;
    ++fd_count;
  }
  closedir(dir);
  return fd_count;
}
#endif

class TurnPortTest : public testing::Test,
                     public sigslot::has_slots<>,
                     public talk_base::MessageHandler {
 public:
  TurnPortTest()
      : main_(talk_base::Thread::Current()),
        pss_(new talk_base::PhysicalSocketServer),
        ss_(new talk_base::VirtualSocketServer(pss_.get())),
        ss_scope_(ss_.get()),
        network_("unittest", "unittest", talk_base::IPAddress(INADDR_ANY), 32),
        socket_factory_(talk_base::Thread::Current()),
        turn_server_(main_, kTurnUdpIntAddr, kTurnUdpExtAddr),
        turn_ready_(false),
        turn_error_(false),
        turn_unknown_address_(false),
        turn_create_permission_success_(false),
        udp_ready_(false),
        test_finish_(false) {
    network_.AddIP(talk_base::IPAddress(INADDR_ANY));
  }

  virtual void OnMessage(talk_base::Message* msg) {
    ASSERT(msg->message_id == MSG_TESTFINISH);
    if (msg->message_id == MSG_TESTFINISH)
      test_finish_ = true;
  }

  void OnTurnPortComplete(Port* port) {
    turn_ready_ = true;
  }
  void OnTurnPortError(Port* port) {
    turn_error_ = true;
  }
  void OnTurnUnknownAddress(PortInterface* port, const SocketAddress& addr,
                            cricket::ProtocolType proto,
                            cricket::IceMessage* msg, const std::string& rf,
                            bool /*port_muxed*/) {
    turn_unknown_address_ = true;
  }
  void OnTurnCreatePermissionResult(TurnPort* port, const SocketAddress& addr,
                                     int code) {
    // Ignoring the address.
    if (code == 0) {
      turn_create_permission_success_ = true;
    }
  }
  void OnTurnReadPacket(Connection* conn, const char* data, size_t size,
                        const talk_base::PacketTime& packet_time) {
    turn_packets_.push_back(talk_base::Buffer(data, size));
  }
  void OnUdpPortComplete(Port* port) {
    udp_ready_ = true;
  }
  void OnUdpReadPacket(Connection* conn, const char* data, size_t size,
                       const talk_base::PacketTime& packet_time) {
    udp_packets_.push_back(talk_base::Buffer(data, size));
  }
  void OnSocketReadPacket(talk_base::AsyncPacketSocket* socket,
                          const char* data, size_t size,
                          const talk_base::SocketAddress& remote_addr,
                          const talk_base::PacketTime& packet_time) {
    turn_port_->HandleIncomingPacket(socket, data, size, remote_addr,
                                     packet_time);
  }
  talk_base::AsyncSocket* CreateServerSocket(const SocketAddress addr) {
    talk_base::AsyncSocket* socket = ss_->CreateAsyncSocket(SOCK_STREAM);
    EXPECT_GE(socket->Bind(addr), 0);
    EXPECT_GE(socket->Listen(5), 0);
    return socket;
  }

  void CreateTurnPort(const std::string& username,
                      const std::string& password,
                      const cricket::ProtocolAddress& server_address) {
    CreateTurnPort(kLocalAddr1, username, password, server_address);
  }
  void CreateTurnPort(const talk_base::SocketAddress& local_address,
                      const std::string& username,
                      const std::string& password,
                      const cricket::ProtocolAddress& server_address) {
    cricket::RelayCredentials credentials(username, password);
    turn_port_.reset(TurnPort::Create(main_, &socket_factory_, &network_,
                                 local_address.ipaddr(), 0, 0,
                                 kIceUfrag1, kIcePwd1,
                                 server_address, credentials));
    // Set ICE protocol type to ICEPROTO_RFC5245, as port by default will be
    // in Hybrid mode. Protocol type is necessary to send correct type STUN ping
    // messages.
    // This TURN port will be the controlling.
    turn_port_->SetIceProtocolType(cricket::ICEPROTO_RFC5245);
    turn_port_->SetIceRole(cricket::ICEROLE_CONTROLLING);
    ConnectSignals();
  }

  void CreateSharedTurnPort(const std::string& username,
                            const std::string& password,
                            const cricket::ProtocolAddress& server_address) {
    ASSERT(server_address.proto == cricket::PROTO_UDP);

    socket_.reset(socket_factory_.CreateUdpSocket(
        talk_base::SocketAddress(kLocalAddr1.ipaddr(), 0), 0, 0));
    ASSERT_TRUE(socket_ != NULL);
    socket_->SignalReadPacket.connect(this, &TurnPortTest::OnSocketReadPacket);

    cricket::RelayCredentials credentials(username, password);
    turn_port_.reset(cricket::TurnPort::Create(
        main_, &socket_factory_, &network_, socket_.get(), kIceUfrag1, kIcePwd1,
        server_address, credentials));
    // Set ICE protocol type to ICEPROTO_RFC5245, as port by default will be
    // in Hybrid mode. Protocol type is necessary to send correct type STUN ping
    // messages.
    // This TURN port will be the controlling.
    turn_port_->SetIceProtocolType(cricket::ICEPROTO_RFC5245);
    turn_port_->SetIceRole(cricket::ICEROLE_CONTROLLING);
    ConnectSignals();
  }

  void ConnectSignals() {
    turn_port_->SignalPortComplete.connect(this,
        &TurnPortTest::OnTurnPortComplete);
    turn_port_->SignalPortError.connect(this,
        &TurnPortTest::OnTurnPortError);
    turn_port_->SignalUnknownAddress.connect(this,
        &TurnPortTest::OnTurnUnknownAddress);
    turn_port_->SignalCreatePermissionResult.connect(this,
        &TurnPortTest::OnTurnCreatePermissionResult);
  }
  void CreateUdpPort() {
    udp_port_.reset(UDPPort::Create(main_, &socket_factory_, &network_,
                                    kLocalAddr2.ipaddr(), 0, 0,
                                    kIceUfrag2, kIcePwd2));
    // Set protocol type to RFC5245, as turn port is also in same mode.
    // UDP port will be controlled.
    udp_port_->SetIceProtocolType(cricket::ICEPROTO_RFC5245);
    udp_port_->SetIceRole(cricket::ICEROLE_CONTROLLED);
    udp_port_->SignalPortComplete.connect(
        this, &TurnPortTest::OnUdpPortComplete);
  }

  void TestTurnConnection() {
    // Create ports and prepare addresses.
    ASSERT_TRUE(turn_port_ != NULL);
    turn_port_->PrepareAddress();
    ASSERT_TRUE_WAIT(turn_ready_, kTimeout);
    CreateUdpPort();
    udp_port_->PrepareAddress();
    ASSERT_TRUE_WAIT(udp_ready_, kTimeout);

    // Send ping from UDP to TURN.
    Connection* conn1 = udp_port_->CreateConnection(
                    turn_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn1 != NULL);
    conn1->Ping(0);
    WAIT(!turn_unknown_address_, kTimeout);
    EXPECT_FALSE(turn_unknown_address_);
    EXPECT_EQ(Connection::STATE_READ_INIT, conn1->read_state());
    EXPECT_EQ(Connection::STATE_WRITE_INIT, conn1->write_state());

    // Send ping from TURN to UDP.
    Connection* conn2 = turn_port_->CreateConnection(
                    udp_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn2 != NULL);
    ASSERT_TRUE_WAIT(turn_create_permission_success_, kTimeout);
    conn2->Ping(0);

    EXPECT_EQ_WAIT(Connection::STATE_WRITABLE, conn2->write_state(), kTimeout);
    EXPECT_EQ(Connection::STATE_READABLE, conn1->read_state());
    EXPECT_EQ(Connection::STATE_READ_INIT, conn2->read_state());
    EXPECT_EQ(Connection::STATE_WRITE_INIT, conn1->write_state());

    // Send another ping from UDP to TURN.
    conn1->Ping(0);
    EXPECT_EQ_WAIT(Connection::STATE_WRITABLE, conn1->write_state(), kTimeout);
    EXPECT_EQ(Connection::STATE_READABLE, conn2->read_state());
  }

  void TestTurnSendData() {
    turn_port_->PrepareAddress();
    EXPECT_TRUE_WAIT(turn_ready_, kTimeout);
    CreateUdpPort();
    udp_port_->PrepareAddress();
    EXPECT_TRUE_WAIT(udp_ready_, kTimeout);
    // Create connections and send pings.
    Connection* conn1 = turn_port_->CreateConnection(
        udp_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    Connection* conn2 = udp_port_->CreateConnection(
        turn_port_->Candidates()[0], Port::ORIGIN_MESSAGE);
    ASSERT_TRUE(conn1 != NULL);
    ASSERT_TRUE(conn2 != NULL);
    conn1->SignalReadPacket.connect(static_cast<TurnPortTest*>(this),
                                    &TurnPortTest::OnTurnReadPacket);
    conn2->SignalReadPacket.connect(static_cast<TurnPortTest*>(this),
                                    &TurnPortTest::OnUdpReadPacket);
    conn1->Ping(0);
    EXPECT_EQ_WAIT(Connection::STATE_WRITABLE, conn1->write_state(), kTimeout);
    conn2->Ping(0);
    EXPECT_EQ_WAIT(Connection::STATE_WRITABLE, conn2->write_state(), kTimeout);

    // Send some data.
    size_t num_packets = 256;
    for (size_t i = 0; i < num_packets; ++i) {
      char buf[256];
      for (size_t j = 0; j < i + 1; ++j) {
        buf[j] = 0xFF - j;
      }
      conn1->Send(buf, i + 1, options);
      conn2->Send(buf, i + 1, options);
      main_->ProcessMessages(0);
    }

    // Check the data.
    ASSERT_EQ_WAIT(num_packets, turn_packets_.size(), kTimeout);
    ASSERT_EQ_WAIT(num_packets, udp_packets_.size(), kTimeout);
    for (size_t i = 0; i < num_packets; ++i) {
      EXPECT_EQ(i + 1, turn_packets_[i].length());
      EXPECT_EQ(i + 1, udp_packets_[i].length());
      EXPECT_EQ(turn_packets_[i], udp_packets_[i]);
    }
  }

 protected:
  talk_base::Thread* main_;
  talk_base::scoped_ptr<talk_base::PhysicalSocketServer> pss_;
  talk_base::scoped_ptr<talk_base::VirtualSocketServer> ss_;
  talk_base::SocketServerScope ss_scope_;
  talk_base::Network network_;
  talk_base::BasicPacketSocketFactory socket_factory_;
  talk_base::scoped_ptr<talk_base::AsyncPacketSocket> socket_;
  cricket::TestTurnServer turn_server_;
  talk_base::scoped_ptr<TurnPort> turn_port_;
  talk_base::scoped_ptr<UDPPort> udp_port_;
  bool turn_ready_;
  bool turn_error_;
  bool turn_unknown_address_;
  bool turn_create_permission_success_;
  bool udp_ready_;
  bool test_finish_;
  std::vector<talk_base::Buffer> turn_packets_;
  std::vector<talk_base::Buffer> udp_packets_;
  talk_base::PacketOptions options;
};

// Do a normal TURN allocation.
TEST_F(TurnPortTest, TestTurnAllocate) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  EXPECT_EQ(0, turn_port_->SetOption(talk_base::Socket::OPT_SNDBUF, 10*1024));
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_ready_, kTimeout);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

TEST_F(TurnPortTest, TestTurnTcpAllocate) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, cricket::PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  EXPECT_EQ(0, turn_port_->SetOption(talk_base::Socket::OPT_SNDBUF, 10*1024));
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_ready_, kTimeout);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

// Try to do a TURN allocation with an invalid password.
TEST_F(TurnPortTest, TestTurnAllocateBadPassword) {
  CreateTurnPort(kTurnUsername, "bad", kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_error_, kTimeout);
  ASSERT_EQ(0U, turn_port_->Candidates().size());
}

// Do a TURN allocation and try to send a packet to it from the outside.
// The packet should be dropped. Then, try to send a packet from TURN to the
// outside. It should reach its destination. Finally, try again from the
// outside. It should now work as well.
TEST_F(TurnPortTest, TestTurnConnection) {
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnConnection();
}

// Similar to above, except that this test will use the shared socket.
TEST_F(TurnPortTest, TestTurnConnectionUsingSharedSocket) {
  CreateSharedTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnConnection();
}

// Test that we can establish a TCP connection with TURN server.
TEST_F(TurnPortTest, TestTurnTcpConnection) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, cricket::PROTO_TCP);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  TestTurnConnection();
}

// Test that we fail to create a connection when we want to use TLS over TCP.
// This test should be removed once we have TLS support.
TEST_F(TurnPortTest, TestTurnTlsTcpConnectionFails) {
  cricket::ProtocolAddress secure_addr(kTurnTcpProtoAddr.address,
                                       kTurnTcpProtoAddr.proto,
                                       true);
  CreateTurnPort(kTurnUsername, kTurnPassword, secure_addr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_error_, kTimeout);
  ASSERT_EQ(0U, turn_port_->Candidates().size());
}

// Run TurnConnectionTest with one-time-use nonce feature.
// Here server will send a 438 STALE_NONCE error message for
// every TURN transaction.
TEST_F(TurnPortTest, TestTurnConnectionUsingOTUNonce) {
  turn_server_.set_enable_otu_nonce(true);
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnConnection();
}

// Do a TURN allocation, establish a UDP connection, and send some data.
TEST_F(TurnPortTest, TestTurnSendDataTurnUdpToUdp) {
  // Create ports and prepare addresses.
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnUdpProtoAddr);
  TestTurnSendData();
}

// Do a TURN allocation, establish a TCP connection, and send some data.
TEST_F(TurnPortTest, TestTurnSendDataTurnTcpToUdp) {
  turn_server_.AddInternalSocket(kTurnTcpIntAddr, cricket::PROTO_TCP);
  // Create ports and prepare addresses.
  CreateTurnPort(kTurnUsername, kTurnPassword, kTurnTcpProtoAddr);
  TestTurnSendData();
}

// Test TURN fails to make a connection from IPv6 address to a server which has
// IPv4 address.
TEST_F(TurnPortTest, TestTurnLocalIPv6AddressServerIPv4) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, cricket::PROTO_UDP);
  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 kTurnUdpProtoAddr);
  turn_port_->PrepareAddress();
  ASSERT_TRUE_WAIT(turn_error_, kTimeout);
  EXPECT_TRUE(turn_port_->Candidates().empty());
}

// Test TURN make a connection from IPv6 address to a server which has
// IPv6 intenal address. But in this test external address is a IPv4 address,
// hence allocated address will be a IPv4 address.
TEST_F(TurnPortTest, TestTurnLocalIPv6AddressServerIPv6ExtenalIPv4) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, cricket::PROTO_UDP);
  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 kTurnUdpIPv6ProtoAddr);
  turn_port_->PrepareAddress();
  EXPECT_TRUE_WAIT(turn_ready_, kTimeout);
  ASSERT_EQ(1U, turn_port_->Candidates().size());
  EXPECT_EQ(kTurnUdpExtAddr.ipaddr(),
            turn_port_->Candidates()[0].address().ipaddr());
  EXPECT_NE(0, turn_port_->Candidates()[0].address().port());
}

// This test verifies any FD's are not leaked after TurnPort is destroyed.
// https://code.google.com/p/webrtc/issues/detail?id=2651
#if defined(LINUX)
TEST_F(TurnPortTest, TestResolverShutdown) {
  turn_server_.AddInternalSocket(kTurnUdpIPv6IntAddr, cricket::PROTO_UDP);
  int last_fd_count = GetFDCount();
  // Need to supply unresolved address to kick off resolver.
  CreateTurnPort(kLocalIPv6Addr, kTurnUsername, kTurnPassword,
                 cricket::ProtocolAddress(talk_base::SocketAddress(
                    "stun.l.google.com", 3478), cricket::PROTO_UDP));
  turn_port_->PrepareAddress();
  ASSERT_TRUE_WAIT(turn_error_, kTimeout);
  EXPECT_TRUE(turn_port_->Candidates().empty());
  turn_port_.reset();
  talk_base::Thread::Current()->Post(this, MSG_TESTFINISH);
  // Waiting for above message to be processed.
  ASSERT_TRUE_WAIT(test_finish_, kTimeout);
  EXPECT_EQ(last_fd_count, GetFDCount());
}
#endif

