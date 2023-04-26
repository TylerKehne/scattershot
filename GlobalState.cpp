#include <Scattershot.hpp>

GlobalState::GlobalState(Configuration& config, Printer& printer) : config(config), printer(printer)
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

void GlobalState::MergeBlocks()
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

void GlobalState::MergeSegments()
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

void GlobalState::SegmentGarbageCollection()
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

void GlobalState::MergeState(int mainIteration)
{
    // Merge all blocks from all threads and redistribute info.
    MergeBlocks();

    // Handle segments
    MergeSegments();

    if (mainIteration % (config.ShotsPerMerge * config.MergesPerSegmentGC) == 0)
        SegmentGarbageCollection();
}