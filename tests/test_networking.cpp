/// Networking layer smoke test.
/// Verifies that Transport / GameServer / GameClient compile and report
/// graceful failures when ENet is not integrated.
#include "net/Transport.h"
#include "net/GameServer.h"
#include "net/GameClient.h"
#include "net/NetTypes.h"
#include <cassert>
#include <cstdio>

using namespace glory;

int main() {
    // Transport stubs should return false (ENet not integrated)
    Transport t;
    assert(!t.startServer(7777));
    assert(!t.connectToServer("127.0.0.1", 7777));
    assert(!t.isConnected());

    // GameServer init
    GameServer server;
    assert(!server.start(7777)); // expected failure — ENet stub

    // GameClient
    GameClient client;
    assert(!client.connect("127.0.0.1", 7777));
    assert(!client.isConnected());

    // NetTypes: verify struct sizes are as expected for wire format
    static_assert(sizeof(PacketType)      == 1);
    static_assert(sizeof(TickConfirmation) >= 12);

    printf("test_networking: all assertions passed (ENet stub mode)\n");
    return 0;
}
