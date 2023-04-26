#include <Scattershot.hpp>
#include <Utils.hpp>
#include <Script.hpp>

void AdvanceToStart(Configuration& config, ThreadState& tState, SaveState& saveState, Dll& dll, Input* fileInputs)
{
    Input* gControllerPads = (Input*)GetProcAddress(dll.hdll, "gControllerPads");
    VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_update");

    for (int f = 0; f < config.StartFrame + 5; f++) {
        *gControllerPads = tState.CurrentInput = fileInputs[f];
        sm64_update();
        if (f == config.StartFrame - 1) saveState.save(dll);
    }
}

int DecodeAndExecuteDiff(Configuration& config, ThreadState& tState, Input* m64Diff, HMODULE& dll)
{
    Input* gControllerPads = (Input*)GetProcAddress(dll, "gControllerPads");
    VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll, "sm64_update");

    int frameOffset = 0;

    //UPDATED FOR SEGMENTS STRUCT
    //Before, I temporarily reversed the linked list.
    //But this doesn't work in a multi threaded environment.
    //Punt and do this in quadratic time. This shouldn't be a
    //bottleneck anyway but can fix it if needed
    if (tState.BaseBlock.tailSeg == 0)
        printf("origBlock has null tailSeg");

    Segment* thisTailSeg = tState.BaseBlock.tailSeg;
    Segment* curSeg;
    int thisSegDepth = thisTailSeg->depth;
    for (int i = 1; i <= thisSegDepth; i++) {
        curSeg = thisTailSeg;
        while (curSeg->depth != i) {  //inefficient but probably doesn't matter
            if (curSeg->parent == 0)
                printf("Parent is null!");
            if (curSeg->parent->depth + 1 != curSeg->depth) { printf("Depths wrong"); }
            curSeg = curSeg->parent;
        }

        //Run the inputs
        uint64_t tmpSeed = curSeg->seed;
        int megaRandom = Utils::xoro_r(&tmpSeed) % 2;
        for (int f = 0; f < curSeg->numFrames; f++) {
            Script::perturbInput(dll, &tState.CurrentInput, &tmpSeed, frameOffset, megaRandom);
            m64Diff[frameOffset++] = tState.CurrentInput;
            *gControllerPads = tState.CurrentInput;
            sm64_update();

            tState.UpdateLightning(config, Script::truncFunc(dll));
        }
    }

    return frameOffset;
}

bool ValidateBlock(Configuration& config, ThreadState& tState, HMODULE& dll, Input* m64Diff, int frame)
{
    void* gMarioStates = GetProcAddress(dll, "gMarioStates");
    void* gObjectPool = GetProcAddress(dll, "gObjectPool");
    void* gCamera = GetProcAddress(dll, "gCamera");

    float* marioX = (float*)((char*)gMarioStates + 60);
    float* marioY = (float*)((char*)gMarioStates + 64);
    float* marioZ = (float*)((char*)gMarioStates + 68);
    unsigned int* marioAction = (unsigned int*)((char*)gMarioStates + 12);
    uint16_t* marioYawFacing = (uint16_t*)((char*)gMarioStates + 46);
    float* marioHSpd = (float*)((char*)gMarioStates + 0x54);
    uint16_t* camYaw = (uint16_t*)((char*)gCamera + 340);
    float* marioYVel = (float*)((char*)gMarioStates + 76);
    float* marioFloorHeight = (float*)((char*)gMarioStates + 0x07C);

    float* pyraXNorm = (float*)((char*)gObjectPool + 84 * 1392 + 324);
    float* pyraYNorm = (float*)((char*)gObjectPool + 84 * 1392 + 328);
    float* pyraZNorm = (float*)((char*)gObjectPool + 84 * 1392 + 332);

    unsigned int actionTrunc = *marioAction & 0x1FF;

    if (*marioX < -2330) return false;
    if (*marioX > -1550) return false;
    if (*marioZ < -1090) return false;
    if (*marioZ > -300) return false;
    if (*marioY > -2760) return false;
    if (*pyraZNorm < -.15 || *pyraXNorm > 0.15) return false; //stay in desired quadrant
    if (actionTrunc != ACT_BRAKE && actionTrunc != ACT_DIVE && actionTrunc != ACT_DIVE_LAND &&
        actionTrunc != ACT_DR && actionTrunc != ACT_DR_LAND && actionTrunc != ACT_FREEFALL &&
        actionTrunc != ACT_FREEFALL_LAND && actionTrunc != ACT_TURNAROUND_1 &&
        actionTrunc != ACT_TURNAROUND_2 && actionTrunc != ACT_WALK) {
        return false;
    } //not useful action, such as lava boost
    if (actionTrunc == ACT_FREEFALL && *marioYVel > -20.0) return false;//freefall without having done nut spot chain
    if (*marioFloorHeight > -3071 && *marioY > *marioFloorHeight + 4 &&
        *marioYVel != 22.0) return false;//above pyra by over 4 units
    if (*marioFloorHeight == -3071 && actionTrunc != ACT_FREEFALL) return false; //diving/dring above lava

    if (actionTrunc == ACT_DR && fabs(*pyraXNorm) > .3 && fabs(*pyraXNorm) + fabs(*pyraZNorm) > .65 &&
        *marioX + *marioZ > (-1945 - 715)) {  //make sure Mario is going toward the right/east edge
        char fileName[128];
        //printf("dr\n");
        sprintf(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\m64s\\dr\\bitfs_dr_%f_%f_%f_%f_%d.m64", *pyraXNorm, *pyraYNorm, *pyraZNorm, *marioYVel, tState.Id);
        Utils::writeFile(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64", m64Diff, config.StartFrame, frame + 1);
    }

    //check on hspd > 1 confirms we're in dr land rather than quickstopping,
    //which gives the same action
    if (actionTrunc == ACT_DR_LAND && *marioY > -2980 && *marioHSpd > 1
        && fabs(*pyraXNorm) > .29 && fabs(*marioX) > -1680) {
        char fileName[128];
        //if(printingDRLand > 0)printf("dr land\n");
        sprintf(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\m64s\\drland\\bitfs_drland_%f_%f_%f_%d.m64", *pyraXNorm, *pyraYNorm, *pyraZNorm, tState.Id);
        Utils::writeFile(fileName, "C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64", m64Diff, config.StartFrame, frame + 1);
    }

    return true;
}

void ExtendTasFromBlock(Configuration& config, GlobalState& gState, ThreadState& tState, HMODULE& dll,
    Input* m64Diff, int frameOffset, int megaRandom, uint64_t baseRngSeed, Vec3d prevStateBin)
{
    VOIDFUNC sm64_update = (VOIDFUNC)GetProcAddress(dll, "sm64_update");
    Input* gControllerPads = (Input*)GetProcAddress(dll, "gControllerPads");

    for (int f = 0; f < config.SegmentLength; f++) {
        Script::perturbInput(dll, &tState.CurrentInput, &tState.RngSeed, frameOffset + f, megaRandom);
        m64Diff[frameOffset + f] = tState.CurrentInput;
        *gControllerPads = tState.CurrentInput;

        auto timerStart = omp_get_wtime();
        sm64_update();
        tState.RunTime += omp_get_wtime() - timerStart;

        if (!tState.ValidateCourseAndArea(dll) || !ValidateBlock(config, tState, dll, m64Diff, frameOffset + f))
            break;

        Vec3d newStateBin = Script::truncFunc(dll);
        tState.UpdateLightning(config, newStateBin);

        //fifd: Checks to see if we're in a new Block. If so, save off the segment so far.
        timerStart = omp_get_wtime();
        if (!newStateBin.truncEq(prevStateBin) && !newStateBin.truncEq(tState.BaseBlock.pos))
        {
            // Create and add block to list.
            tState.ProcessNewBlock(config, gState, baseRngSeed, f, newStateBin, Script::StateBinFitness(dll));

            prevStateBin = newStateBin; // TODO: Why this here?
        }
        tState.BlockTime += omp_get_wtime() - timerStart;
    }
}

void InitConfiguration(Configuration& configuration)
{
    configuration.StartFrame = 3545;
    configuration.SegmentLength = 10;
    configuration.MaxSegments = 1024;
    configuration.MaxBlocks = 500000;
    configuration.MaxHashes = 10 * configuration.MaxBlocks;
    configuration.MaxSharedBlocks = 20000000;
    configuration.MaxSharedHashes = 10 * configuration.MaxSharedBlocks;
    configuration.TotalThreads = 4;
    configuration.MaxSharedSegments = 25000000;
    configuration.MaxLocalSegments = 2000000;
    configuration.MaxLightningLength = 10000;
}

void main(int argc, char* argv[])
{
    Printer printer;
    printer.ParseArgs(argc, argv);

    Configuration config;
    InitConfiguration(config);
    GlobalState gState = GlobalState(config);

    omp_set_num_threads(config.TotalThreads);
    Utils::MultiThread([&]()
        {
            //TODO: Maybe don't hardcode DLLs
            LPCWSTR dlls[4] = { L"sm64_jp_0.dll", L"sm64_jp_1.dll" , L"sm64_jp_2.dll" , L"sm64_jp_3.dll" };
            ThreadState tState = ThreadState(config, gState, omp_get_thread_num());
            Dll dll;
            dll.getDllInfo(dlls[tState.Id]);
            SaveState state, state2;
            state.allocState(dll);
            state2.allocState(dll);
            Input* m64Diff = (Input*)malloc(sizeof(Input) * (config.SegmentLength * config.MaxSegments + 256)); // Todo: Nasty

            // Initialize game
            VOIDFUNC sm64_init = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_init");
            sm64_init();

            // Read inputs from file and advance to start frame
            Input* fileInputs = Utils::GetM64("C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64");
            AdvanceToStart(config, tState, state, dll, fileInputs);
            tState.LoadTime += state.riskyLoadJ(dll);

            // Initialize thread state
            tState.Initialize(config, gState, Script::truncFunc(dll.hdll), dll.hdll);

            for (int mainIteration = 0; mainIteration <= 1000000000; mainIteration++) {
                // ALWAYS START WITH A MERGE SO THE SHARED BLOCKS ARE OK.
                if (mainIteration % 300 == 0)
                    Utils::SingleThread([&]()
                        { 
                            gState.MergeState(config, mainIteration, printer);
                            tState.PrintStatus(config, gState, mainIteration, printer);
                        });

                // Pick a block to "fire a scattershot" at
                if (!tState.SelectBaseBlock(config, gState, mainIteration))
                    break;

                // Revert to initial state, and advance game state to end of block diff
                tState.LoadTime += state.riskyLoadJ(dll);
                tState.LightningLengthLocal = 0;
                tState.LightningLocal[tState.LightningLengthLocal++] = Script::truncFunc(dll.hdll);
                int frameOffset = DecodeAndExecuteDiff(config, tState, m64Diff, dll.hdll);
                state2.save(dll);

                // Sanity check that state matches saved block state
                if (!tState.ValidateBaseBlock(Script::truncFunc(dll.hdll)))
                    return;

                // "Fire" the scattershot, i.e. execute a batch of semi-random input sequences from the base block state.
                Input origLastIn = tState.CurrentInput;
                int origLightLenLocal = tState.LightningLengthLocal;
                for (int subLoop = 0; subLoop < 200; subLoop++) {
                    tState.LoadTime += state2.riskyLoadJ(dll);

                    tState.CurrentInput = origLastIn;
                    tState.LightningLengthLocal = origLightLenLocal;

                    uint64_t baseRngSeed = tState.RngSeed;
                    int megaRandom = Utils::xoro_r(&tState.RngSeed) % 2;
                    ExtendTasFromBlock(config, gState, tState, dll.hdll, m64Diff, frameOffset, megaRandom, baseRngSeed, tState.BaseBlock.pos);
                }
            }
        });
}
