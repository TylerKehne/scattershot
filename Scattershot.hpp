#pragma once
#include "Utils.hpp"

#ifndef SCATTERSHOT_H
#define SCATTERSHOT_H

typedef struct Segment Segment;

struct Segment {
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

    int findNewHashInx(int* hashTab, int maxHashes) {
        uint64_t tmpSeed = hashPos();
        for (int i = 0; i < 100; i++) {
            int inx = tmpSeed % maxHashes;
            if (hashTab[inx] == -1) return inx;
            Utils::xoro_r(&tmpSeed);
        }
        printf("Failed to find new hash index after 100 tries!\n");
        return -1;
    }

    uint64_t hashPos() {
        uint64_t tmpSeed = 0xCABBA6ECABBA6E;
        tmpSeed += x + 0xCABBA6E;
        Utils::xoro_r(&tmpSeed);
        tmpSeed += y + 0xCABBA6E;
        Utils::xoro_r(&tmpSeed);
        tmpSeed += z + 0xCABBA6E;
        Utils::xoro_r(&tmpSeed);
        tmpSeed += s + 0xCABBA6E;
        Utils::xoro_r(&tmpSeed);
        return tmpSeed;
    }

    //fifd: Check equality of truncated states
    int truncEq(Vec3d b) {
        return (x == b.x) && (y == b.y) && (z == b.z) && (s == b.s);
    }

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

    //UPDATED FOR SEGMENT STRUCT
    int blockLength() {
        int len = 0;
        Segment* curSeg = tailSeg;
        if (tailSeg->depth == 0) { printf("tailSeg depth is 0!\n"); }
        while (curSeg != 0) {
            if (curSeg->depth == 0) { printf("curSeg depth is 0!\n"); }
            len += curSeg->numFrames;
            curSeg = curSeg->parent;
        }
        return len;
    }
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

    GlobalState(Configuration& config)
    {
        AllBlocks = (Block*)calloc(config.TotalThreads * config.MaxBlocks + config.MaxSharedBlocks, sizeof(Block));
        AllSegments = (struct Segment**)malloc((config.MaxSharedSegments + config.TotalThreads * config.MaxLocalSegments) * sizeof(struct Segment*));
        AllHashTabs = (int*)calloc(config.TotalThreads * config.MaxHashes + config.MaxSharedHashes, sizeof(int));
        NBlocks = (int*)calloc(config.TotalThreads + 1, sizeof(int));
        NSegments = (int*)calloc(config.TotalThreads + 1, sizeof(int));
        SharedBlocks = AllBlocks + config.TotalThreads * config.MaxBlocks;
        SharedHashTab = AllHashTabs + config.TotalThreads * config.MaxHashes;

        // Init shared hash table.
        for (int hashInx = 0; hashInx < config.MaxSharedHashes; hashInx++)
            SharedHashTab[hashInx] = -1;
    }

    void MergeBlocks(Configuration& config, Printer& printer)
    {
        printer.printfQ("Merging blocks.\n");

        int otid, n, m;
        for (otid = 0; otid < config.TotalThreads; otid++) {
            for (n = 0; n < NBlocks[otid]; n++) {
                Block tmpBlock = AllBlocks[otid * config.MaxBlocks + n];
                m = tmpBlock.pos.findBlock(SharedBlocks, SharedHashTab, config.MaxSharedHashes, 0, NBlocks[config.TotalThreads]);
                if (m < NBlocks[config.TotalThreads]) {
                    if (tmpBlock.value > SharedBlocks[m].value) { // changed to >
                        SharedBlocks[m] = tmpBlock;
                    }
                }
                else {
                    SharedHashTab[tmpBlock.pos.findNewHashInx(SharedHashTab, config.MaxSharedHashes)] = NBlocks[config.TotalThreads];
                    SharedBlocks[NBlocks[config.TotalThreads]++] = tmpBlock;
                }
            }
        }

        memset(AllHashTabs, 0xFF, config.MaxHashes * config.TotalThreads * sizeof(int)); // Clear all local hash tables.

        for (otid = 0; otid < config.TotalThreads; otid++) {
            NBlocks[otid] = 0; // Clear all local blocks.
        }
    }

    void MergeSegments(Configuration& config)
    {
        printf("Merging segments\n");

        // Get reference counts for each segment. Tried to track this but ran into
        // multi-threading issues, so might as well recompute here.
        for (int threadNum = 0; threadNum < config.TotalThreads; threadNum++) {
            for (int segInd = threadNum * config.MaxLocalSegments; segInd < threadNum * config.MaxLocalSegments + NSegments[threadNum]; segInd++) {
                //printf("%d %d\n", segInd, numSegs[totThreads]);
                AllSegments[config.TotalThreads * config.MaxLocalSegments + NSegments[config.TotalThreads]] = AllSegments[segInd];
                NSegments[config.TotalThreads]++;
                AllSegments[segInd] = 0;
            }
            NSegments[threadNum] = 0;
        }
    }

    void SegmentGarbageCollection(Configuration& config)
    {
        printf("Segment garbage collection. Start with %d segments\n", NSegments[config.TotalThreads]);

        for (int segInd = config.TotalThreads * config.MaxLocalSegments; segInd < config.TotalThreads * config.MaxLocalSegments + NSegments[config.TotalThreads]; segInd++) {
            AllSegments[segInd]->refCount = 0;
        }
        for (int segInd = config.TotalThreads * config.MaxLocalSegments; segInd < config.TotalThreads * config.MaxLocalSegments + NSegments[config.TotalThreads]; segInd++) {
            if (AllSegments[segInd]->parent != 0) { AllSegments[segInd]->parent->refCount++; }
        }
        for (int blockInd = 0; blockInd < NBlocks[config.TotalThreads]; blockInd++) {
            SharedBlocks[blockInd].tailSeg->refCount++;
        }
        for (int segInd = config.TotalThreads * config.MaxLocalSegments; segInd < config.TotalThreads * config.MaxLocalSegments + NSegments[config.TotalThreads]; segInd++) {
            Segment* curSeg = AllSegments[segInd];
            if (curSeg->refCount == 0) {
                //printf("removing a seg\n");
                if (curSeg->parent != 0) { curSeg->parent->refCount -= 1; }
                //printf("moving %d %d\n", segInd, totThreads*maxLocalSegs+numSegs[totThreads]);
                AllSegments[segInd] = AllSegments[config.TotalThreads * config.MaxLocalSegments + NSegments[config.TotalThreads] - 1];
                NSegments[config.TotalThreads]--;
                segInd--;
                free(curSeg);
            }
        }

        printf("Segment garbage collection finished. Ended with %d segments\n", NSegments[config.TotalThreads]);
    }

    void MergeState(Configuration& config, int mainIteration, Printer& printer)
    {
        // Merge all blocks from all threads and redistribute info.
        MergeBlocks(config, printer);

        // Handle segments
        MergeSegments(config);

        if (mainIteration % 3000 == 0)
            SegmentGarbageCollection(config);
    }
};

class ThreadState
{
public:
    Block* Blocks;
    int* HashTab;
    int Id;
    uint64_t RngSeed;

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

    ThreadState(Configuration& config, GlobalState& gState, int id)
    {
        Id = id;
        Blocks = gState.AllBlocks + Id * config.MaxBlocks;
        HashTab = gState.AllHashTabs + Id * config.MaxHashes;
        RngSeed = (uint64_t)(Id + 173) * 5786766484692217813;

        printf("Thread %d\n", Id);
    }

    void Initialize(Configuration& config, GlobalState& gState, Vec3d initTruncPos)
    {
        // Initial block
        Blocks[0].pos = initTruncPos; //CHEAT TODO NOTE
        Blocks[0].tailSeg = (Segment*)malloc(sizeof(Segment)); //Instantiate root segment
        Blocks[0].tailSeg->numFrames = 0;
        Blocks[0].tailSeg->parent = NULL;
        Blocks[0].tailSeg->refCount = 0;
        Blocks[0].tailSeg->depth = 1;

        // Init local hash table.
        for (int hashInx = 0; hashInx < config.MaxHashes; hashInx++)
            HashTab[hashInx] = -1;

        HashTab[Blocks[0].pos.findNewHashInx(HashTab, config.MaxHashes)] = 0;

        // Lightning
        LightningLength = 0;
        LightningLengthLocal = 0;
        Lightning = (Vec3d*)malloc(sizeof(Vec3d) * config.MaxLightningLength);
        LightningLocal = (Vec3d*)malloc(sizeof(Vec3d) * config.MaxLightningLength);

        // Synchronize global state
        gState.AllSegments[gState.NSegments[Id] + Id * config.MaxLocalSegments] = Blocks[0].tailSeg;
        gState.NSegments[Id]++;
        gState.NBlocks[Id]++;

        LoopTimeStamp = omp_get_wtime();
    }

    bool SelectBaseBlock(Configuration& config, GlobalState& gState, int mainIteration)
    {
        int origInx = gState.NBlocks[config.TotalThreads];
        if (mainIteration % 15 == 0) {
            origInx = 0;
        }
        else if (mainIteration % 7 == 1 && LightningLength > 0) {
            for (int attempt = 0; attempt < 1000; attempt++) {
                int randomLightInx = Utils::xoro_r(&RngSeed) % LightningLength;
                origInx = Lightning[randomLightInx].findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, 0, gState.NBlocks[config.TotalThreads]);
                if (origInx != gState.NBlocks[config.TotalThreads]) break;
            }
            if (origInx == gState.NBlocks[config.TotalThreads]) {
                printf("Could not find lightning block, using root!\n");
                origInx = 0;
            }
        }
        else {
            int weighted = Utils::xoro_r(&RngSeed) % 5;
            for (int attempt = 0; attempt < 100000; attempt++) {
                origInx = Utils::xoro_r(&RngSeed) % gState.NBlocks[config.TotalThreads];
                if (gState.SharedBlocks[origInx].tailSeg == 0) { printf("Chosen block tailseg null!\n"); continue; }
                if (gState.SharedBlocks[origInx].tailSeg->depth == 0) { printf("Chosen block tailseg depth 0!\n"); continue; }
                uint64_t s = gState.SharedBlocks[origInx].pos.s;
                int normInfo = s % 900;
                float xNorm = (float)((int)normInfo / 30);
                float zNorm = (float)(normInfo % 30);
                float approxXZSum = fabs((xNorm - 15) / 15) + fabs((zNorm - 15) / 15) + .01;
                if (((float)(Utils::xoro_r(&RngSeed) % 50) / 100 < approxXZSum * approxXZSum) & (gState.SharedBlocks[origInx].tailSeg->depth < config.MaxSegments)) break;
            }
            if (origInx == gState.NBlocks[config.TotalThreads]) {
                printf("Could not find block!\n");
                return false;
            }
        }

        BaseBlock = gState.SharedBlocks[origInx];
        if (BaseBlock.tailSeg->depth > config.MaxSegments + 2) { printf("BaseBlock depth above max!\n"); }
        if (BaseBlock.tailSeg->depth == 0) { printf("BaseBlock depth is zero!\n"); }

        return true;
    }

    void UpdateLightning(Configuration& config, Vec3d stateBin)
    {
        if (!stateBin.truncEq(LightningLocal[LightningLengthLocal - 1])) {
            if (LightningLengthLocal < config.MaxLightningLength) {
                LightningLocal[LightningLengthLocal++] = stateBin;
            }
            else {
                printf("Reached max lightning!\n");
            }
        }
    }

    bool ValidateBaseBlock(Vec3d baseBlockStateBin)
    {
        if (!BaseBlock.pos.truncEq(baseBlockStateBin)) {
            printf("ORIG %d %d %d %ld AND BLOCK %d %d %d %ld NOT EQUAL\n",
                baseBlockStateBin.x, baseBlockStateBin.y, baseBlockStateBin.z, baseBlockStateBin.s,
                BaseBlock.pos.x, BaseBlock.pos.y, BaseBlock.pos.z, BaseBlock.pos.s);

            Segment* curSegDebug = BaseBlock.tailSeg;
            while (curSegDebug != 0) {  //inefficient but probably doesn't matter
                if (curSegDebug->parent == 0)
                    printf("Parent is null!");
                if (curSegDebug->parent->depth + 1 != curSegDebug->depth) { printf("Depths wrong"); }
                curSegDebug = curSegDebug->parent;
            }

            return false;
        }

        return true;
    }

    void ProcessNewBlock(Configuration& config, GlobalState& gState, uint64_t prevRngSeed, int nFrames, Vec3d newPos, float newFitness)
    {
        Block newBlock;

        // Create and add block to list.
        if (gState.NBlocks[Id] == config.MaxBlocks) {
            printf("Max local blocks reached!\n");
        }
        else {
            //UPDATED FOR SEGMENTS STRUCT
            newBlock = BaseBlock;
            newBlock.pos = newPos;
            newBlock.value = newFitness;
            int blInxLocal = newPos.findBlock(Blocks, HashTab, config.MaxHashes, 0, gState.NBlocks[Id]);
            int blInx = newPos.findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, 0, gState.NBlocks[config.TotalThreads]);

            if (blInxLocal < gState.NBlocks[Id]) { // Existing local block.
                if (newBlock.value >= Blocks[blInxLocal].value) {
                    Segment* newSeg = (Segment*)malloc(sizeof(Segment));
                    newSeg->parent = BaseBlock.tailSeg;
                    newSeg->refCount = 0;
                    newSeg->numFrames = nFrames + 1;
                    newSeg->seed = prevRngSeed;
                    newSeg->depth = BaseBlock.tailSeg->depth + 1;
                    if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
                    if (BaseBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
                    newBlock.tailSeg = newSeg;
                    gState.AllSegments[Id * config.MaxLocalSegments + gState.NSegments[Id]] = newSeg;
                    gState.NSegments[Id] += 1;
                    Blocks[blInxLocal] = newBlock;
                }
            }
            else if (blInx < gState.NBlocks[config.TotalThreads] && newBlock.value < gState.SharedBlocks[blInx].value);// Existing shared block but worse.
            else { // Existing shared block and better OR completely new block.
                HashTab[newPos.findNewHashInx(HashTab, config.MaxHashes)] = gState.NBlocks[Id];
                Segment* newSeg = (Segment*)malloc(sizeof(Segment));
                newSeg->parent = BaseBlock.tailSeg;
                newSeg->refCount = 1;
                newSeg->numFrames = nFrames + 1;
                newSeg->seed = prevRngSeed;
                newSeg->depth = BaseBlock.tailSeg->depth + 1;
                if (newSeg->depth == 0) { printf("newSeg depth is 0!\n"); }
                if (BaseBlock.tailSeg->depth == 0) { printf("origBlock tailSeg depth is 0!\n"); }
                newBlock.tailSeg = newSeg;
                gState.AllSegments[Id * config.MaxLocalSegments + gState.NSegments[Id]] = newSeg;
                gState.NSegments[Id] += 1;
                Blocks[gState.NBlocks[Id]++] = newBlock;
            }
        }
    }

    void PrintStatus(Configuration& config, GlobalState& gState, int mainIteration, Printer& printer)
    {
        printer.printfQ("\nThread ALL Loop %d blocks %d\n", mainIteration, gState.NBlocks[config.TotalThreads]);
        printer.printfQ("LOAD %.3f RUN %.3f BLOCK %.3f TOTAL %.3f\n", LoadTime, RunTime, BlockTime, omp_get_wtime() - LoopTimeStamp);
        printer.printfQ("\n\n");

        LoadTime = RunTime = BlockTime = 0;
        LoopTimeStamp = omp_get_wtime();

        printer.flushLog();
    }
};

#endif
