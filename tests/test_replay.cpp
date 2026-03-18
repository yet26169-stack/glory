#include "replay/ReplayRecorder.h"
#include "replay/ReplayPlayer.h"
#include "network/InputSync.h"

#include <cassert>
#include <cstdio>
#include <vector>
#include <filesystem>

using namespace glory;

void test_record_playback() {
    const std::string filename = "test_replay.rply";
    
    // 1. Record
    {
        ReplayRecorder recorder;
        bool ok = recorder.begin(filename, 1, 12345);
        assert(ok);

        CollectedInputFrame frame;
        frame.tick = 0;
        frame.playerCount = 1;
        frame.inputs[0].buttons = 0x1;
        frame.inputs[0].moveAngle = 180;

        recorder.recordTick(0, frame);
        
        frame.tick = 1;
        recorder.recordTick(1, frame);
        
        recorder.end(999);
    }

    // 2. Playback
    {
        ReplayPlayer player;
        bool ok = player.load(filename);
        if (!ok) {
            printf("  FAIL: ReplayPlayer::load failed\n");
            assert(false);
        }
        
        // ReplayPlayer should have 2 frames
        if (player.totalTicks() != 2) {
            printf("  FAIL: expected 2 ticks, got %u\n", player.totalTicks());
            assert(false);
        }
        
        assert(player.header().rngSeed == 12345);
        assert(player.finalChecksum() == 999);

        CollectedInputFrame out;
        bool next = player.nextTick(out);
        assert(next);
        assert(out.tick == 0);
        assert(out.inputs[0].buttons == 0x1);
        assert(out.inputs[0].moveAngle == 180);

        next = player.nextTick(out);
        assert(next);
        assert(out.tick == 1);
        
        next = player.nextTick(out);
        assert(!next); // End of replay
    }

    std::filesystem::remove(filename);
    printf("  PASS: test_record_playback\n");
}

int main() {
    printf("=== Replay System Tests ===\n");
    test_record_playback();
    return 0;
}
