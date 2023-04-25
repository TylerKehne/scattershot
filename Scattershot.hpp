#pragma once
#include <wtypes.h>
#include <cstdint>
#include <cstdio>

char dataMap[8192] = "  0 ...........................X...........XX.XXX.X....................................................."
"  1 ...................................................................................................."
"  2 ...................................................................................................."
"  3 ...................................................................................................."
"  4 ...................................................................................................."
"  5 ...................................................................................................."
"  6 ...................................................................................................."
"  7 ...................................................................................................."
"  8 ...................................................................................................."
"  9 ...................................................................................................."
" 10 ...................................................................................................."
" 11 ...................................................................................................."
" 12 ...................................................................................................."
" 13 ...................................................................................................."
" 14 ...................................................................................................."
" 15 ...................................................................................................."
" 16 ...................................................................................................."
" 17 ...................................................................................................."
" 18 ...................................................................................................."
" 19 ...................................................................................................."
" 20 ...................................................................................................."
" 21 ...................................................................................................."
" 22 ...................................................................................................."
" 23 ...................................................................................................."
" 24 ...................................................................................................."
" 25 ...........................X........................................................................";

char bssMap[8192] = "  0 XX.....XXX.X................XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
"  1 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
"  2 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
"  3 XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.XXXXXX................XXXXX.."
"  4 ...............................X.......XXXXXXXXXXXX................................................."
"  5 ..................XXXXXXXXXXXXXXXXXXXXXXXX.........................................................."
"  6 ...................................................................................................."
"  7 ...................................................................................................."
"  8 ...................................................................................................."
"  9 ...................................................................................................."
" 10 ...................................................................................................."
" 11 ...................................................................................................."
" 12 ...................................................................................................."
" 13 ...................................................................................................."
" 14 ...................................................................................................."
" 15 ...................................................................................................."
" 16 ...................................................................................................."
" 17 .....................................................................XX............................."
" 18 ..................XXXX....X.......XXXXXXXX...............XXXX...........X.XXX..........XXXX........."
" 19 ................XXX..X.X...X.X..XX.X..........X...X.........XX......................................"
" 20 ........................XX.........................................................................."
" 21 ...........................XX......................................................................."
" 22 ......XXXXXX........................................................................................";

typedef void (CALLBACK* VOIDFUNC)();
typedef void* (CALLBACK* GFXFUNC)(int, void*, void*, void*);

typedef struct {
    unsigned short b;
    char x;
    char y;
} Input;

typedef struct {
    void* data;
    void* bss;
} SaveState;

typedef struct Segment Segment;

struct Segment {
    Segment* parent;
    uint64_t seed;
    uint32_t refCount;
    uint8_t numFrames;
    uint8_t depth;
};

//fifd: Vec3d actually has 4 dimensions, where the first 3 are spatial and
//the 4th encodes a lot of information about Mario's state (actions, speed,
//camera) as well as some button information. These vectors specify a part
//of state space.
typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint64_t s;
} Vec3d;

//fifd: I think this is an element of the partition of state
//space. Will need to understand what each of its fields are
typedef struct {
    Vec3d         pos;    //fifd: an output of truncFunc. Identifies which block this is
    float         value;    //fifd: a fitness of the best TAS that reaches this block; the higher the better.
    Segment* tailSeg;
    //Time is most important component of value but keeps higher hspeed if time is tied
} Block;

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
};

int dataStart, dataLength, bssStart, bssLength;
int gPrint = 1, gLog = 1;
char gProgName[192] = { 0 };
FILE* gLogFP = NULL;

void printfQ(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (gPrint) vprintf(format, args);
    if (gLog) {
        if (!gLogFP) {
            char logName[256] = { 0 };
            sprintf(logName, "%s_log.txt", gProgName);
            gLogFP = fopen(logName, "a");
        }
        vfprintf(gLogFP, format, args);
    }
    va_end(args);
}

void flushLog() {
    fclose(gLogFP);
    gLogFP = NULL;
}

//fifd: only called by xoro_r
static inline uint32_t rotl(const uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

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

    int StartCourse;
    int StartArea;
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

    void Initialize(Configuration& config, GlobalState& gState, Vec3d initTruncPos, HMODULE& dll)
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

        HashTab[findNewHashInx(HashTab, config.MaxHashes, Blocks[0].pos)] = 0;

        // Lightning
        LightningLength = 0;
        LightningLengthLocal = 0;
        Lightning = (Vec3d*)malloc(sizeof(Vec3d) * config.MaxLightningLength);
        LightningLocal = (Vec3d*)malloc(sizeof(Vec3d) * config.MaxLightningLength);

        // Synchronize global state
        gState.AllSegments[gState.NSegments[Id] + Id * config.MaxLocalSegments] = Blocks[0].tailSeg;
        gState.NSegments[Id]++;
        gState.NBlocks[Id]++;

        // Record start course/area for validation (generally scattershot has no cross-level value)
        StartCourse = *(short*)GetProcAddress(dll, "gCurrCourseNum");
        StartArea = *(short*)GetProcAddress(dll, "gCurrAreaIndex");

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
                int randomLightInx = xoro_r(&RngSeed) % LightningLength;
                origInx = findBlock(gState.SharedBlocks, gState.SharedHashTab, config.MaxSharedHashes, Lightning[randomLightInx], 0, gState.NBlocks[config.TotalThreads]);
                if (origInx != gState.NBlocks[config.TotalThreads]) break;
            }
            if (origInx == gState.NBlocks[config.TotalThreads]) {
                printf("Could not find lightning block, using root!\n");
                origInx = 0;
            }
        }
        else {
            int weighted = xoro_r(&RngSeed) % 5;
            for (int attempt = 0; attempt < 100000; attempt++) {
                origInx = xoro_r(&RngSeed) % gState.NBlocks[config.TotalThreads];
                if (gState.SharedBlocks[origInx].tailSeg == 0) { printf("Chosen block tailseg null!\n"); continue; }
                if (gState.SharedBlocks[origInx].tailSeg->depth == 0) { printf("Chosen block tailseg depth 0!\n"); continue; }
                uint64_t s = gState.SharedBlocks[origInx].pos.s;
                int normInfo = s % 900;
                float xNorm = (float)((int)normInfo / 30);
                float zNorm = (float)(normInfo % 30);
                float approxXZSum = fabs((xNorm - 15) / 15) + fabs((zNorm - 15) / 15) + .01;
                if (((float)(xoro_r(&RngSeed) % 50) / 100 < approxXZSum * approxXZSum) & (gState.SharedBlocks[origInx].tailSeg->depth < config.MaxSegments)) break;
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
        if (!truncEq(stateBin, LightningLocal[LightningLengthLocal - 1])) {
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
        if (!truncEq(BaseBlock.pos, baseBlockStateBin)) {
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

    bool ValidateCourseAndArea(HMODULE& dll)
    {
        return StartCourse == *(short*)GetProcAddress(dll, "gCurrCourseNum")
            && StartArea == *(short*)GetProcAddress(dll, "gCurrAreaIndex");
    }
};