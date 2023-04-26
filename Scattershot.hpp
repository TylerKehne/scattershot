#pragma once
#include "Utils.hpp"

#ifndef SCATTERSHOT_H
#define SCATTERSHOT_H

typedef struct Segment Segment;

struct Segment
{
    Segment* parent;
    uint64_t seed;
    uint32_t refCount;
    uint8_t numFrames;
    uint8_t depth;
};

class Block;

//fifd: Vec3d actually has 4 dimensions, where the first 3 are spatial and
//the 4th encodes a lot of information about Mario's state (actions, speed,
//camera) as well as some button information. These vectors specify a part
//of state space.
class Vec3d {
public:
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint64_t s;

    int findNewHashInx(int* hashTab, int maxHashes);
    uint64_t hashPos();
    int truncEq(Vec3d b);
    int findBlock(Block* blocks, int* hashTab, int maxHashes, int nMin, int nMax);
};

//fifd: I think this is an element of the partition of state
//space. Will need to understand what each of its fields are
class Block {
public:
    Vec3d         pos;    //fifd: an output of truncFunc. Identifies which block this is
    float         value;    //fifd: a fitness of the best TAS that reaches this block; the higher the better.
    Segment* tailSeg;
    //Time is most important component of value but keeps higher hspeed if time is tied

    int blockLength();
};

typedef struct {
    float x, y, z;
    int actTrunc;
    unsigned short yawFacing;
    float hspd;
} FinePos;

class Configuration
{
public:
    int StartFrame;
    int SegmentLength;
    int MaxSegments;
    int MaxBlocks;
    int MaxHashes;
    int MaxSharedBlocks;
    int MaxSharedHashes;
    int TotalThreads;
    int MaxSharedSegments;
    int MaxLocalSegments;
    int MaxLightningLength;
    long long MaxShots;
    int SegmentsPerShot;
    int ShotsPerMerge;
    int MergesPerSegmentGC;
};

class GlobalState
{
public:
    struct Segment** AllSegments;
    Block* AllBlocks;
    int* AllHashTabs;
    int* NBlocks;
    int* NSegments;
    Block* SharedBlocks;
    int* SharedHashTab;
    Configuration& config;
    Printer& printer;

    GlobalState(Configuration& config, Printer& printer);

    void MergeState(int mainIteration);
    void MergeBlocks();
    void MergeSegments();
    void SegmentGarbageCollection();
};

class ThreadState
{
public:
    Block* Blocks;
    int* HashTab;
    int Id;
    uint64_t RngSeed;
    Configuration& config;
    GlobalState& gState;

    Block BaseBlock;
    Vec3d BaseStateBin;
    Input CurrentInput;


    double LoadTime = 0;
    double BlockTime = 0;
    double RunTime = 0;
    double LoopTimeStamp = 0;

    // TODO: Unimplemented?
    int LightningLength;
    int LightningLengthLocal;
    Vec3d* Lightning;
    Vec3d* LightningLocal;

    ThreadState(Configuration& config, GlobalState& gState, int id);
    void Initialize(Vec3d initTruncPos);
    bool SelectBaseBlock(int mainIteration);
    void UpdateLightning(Vec3d stateBin);
    bool ValidateBaseBlock(Vec3d baseBlockStateBin);
    void ProcessNewBlock(uint64_t prevRngSeed, int nFrames, Vec3d newPos, float newFitness);
    void PrintStatus(int mainIteration);
};

#endif
