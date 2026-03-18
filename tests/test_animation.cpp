#include "animation/AnimationPlayer.h"
#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"

#include <glm/glm.hpp>
#include <cassert>
#include <cstdio>

using namespace glory;

Skeleton create_test_skeleton() {
    Skeleton skel;
    // Simple 2-bone chain
    Joint j0;
    j0.name = "Root";
    j0.parentIndex = -1;
    j0.localTranslation = {0,0,0};
    skel.joints.push_back(j0);

    Joint j1;
    j1.name = "Bone1";
    j1.parentIndex = 0;
    j1.localTranslation = {0,1,0};
    skel.joints.push_back(j1);

    return skel;
}

AnimationClip create_test_clip(const std::string& name, float duration) {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.looping = true;

    // Animate Bone1 translation from (0,1,0) to (0,2,0)
    AnimationChannel ch;
    ch.targetJointIndex = 1;
    ch.path = AnimationPath::Translation;
    ch.timestamps = {0.0f, duration};
    ch.translationKeys = {{0,1,0}, {0,2,0}};
    clip.channels.push_back(ch);

    return clip;
}

void test_playback() {
    Skeleton skel = create_test_skeleton();
    AnimationClip clip = create_test_clip("Test", 1.0f);
    
    AnimationPlayer player;
    player.setSkeleton(&skel);
    player.setClip(&clip);

    player.update(0.0f); // Time 0
    // We can't easily check localPoses as they are private, 
    // but we can check if it crashes or if skinning matrices are generated.
    assert(player.isPlaying());
    assert(player.getSkinningMatrices().size() == 2);

    player.update(0.5f); // Halfway
    player.update(0.5f); // End
    player.update(0.1f); // Wrap around (looping)

    printf("  PASS: test_playback\n");
}

void test_blending() {
    Skeleton skel = create_test_skeleton();
    AnimationClip clip1 = create_test_clip("Clip1", 1.0f);
    AnimationClip clip2 = create_test_clip("Clip2", 1.0f);

    AnimationPlayer player;
    player.setSkeleton(&skel);
    player.setClip(&clip1);
    
    player.crossfadeTo(&clip2, 0.5f);
    assert(player.isBlending());

    player.update(0.25f);
    assert(player.isBlending());

    player.update(0.3f);
    assert(!player.isBlending()); // Finished 0.25 + 0.3 = 0.55 > 0.5

    printf("  PASS: test_blending\n");
}

int main() {
    printf("=== Animation System Tests ===\n");
    test_playback();
    test_blending();
    return 0;
}
