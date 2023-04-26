#include <Scattershot.hpp>

ThreadState::ThreadState(Configuration& config, GlobalState& gState, int id) : config(config), gState(gState)
{
    Id = id;
    Blocks = gState.AllBlocks + Id * config.MaxBlocks;
    HashTab = gState.AllHashTabs + Id * config.MaxHashes;
    RngSeed = (uint64_t)(Id + 173) * 5786766484692217813;

    printf("Thread %d\n", Id);
}

void ThreadState::Initialize(Vec3d initTruncPos)
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

bool ThreadState::SelectBaseBlock(int mainIteration)
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

void ThreadState::UpdateLightning(Vec3d stateBin)
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

bool ThreadState::ValidateBaseBlock(Vec3d baseBlockStateBin)
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

void ThreadState::ProcessNewBlock(uint64_t prevRngSeed, int nFrames, Vec3d newPos, float newFitness)
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

void ThreadState::PrintStatus(int mainIteration)
{
    gState.printer.printfQ("\nThread ALL Loop %d blocks %d\n", mainIteration, gState.NBlocks[config.TotalThreads]);
    gState.printer.printfQ("LOAD %.3f RUN %.3f BLOCK %.3f TOTAL %.3f\n", LoadTime, RunTime, BlockTime, omp_get_wtime() - LoopTimeStamp);
    gState.printer.printfQ("\n\n");

    LoadTime = RunTime = BlockTime = 0;
    LoopTimeStamp = omp_get_wtime();

    gState.printer.flushLog();
}