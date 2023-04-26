#include <Scattershot.hpp>
#include <Utils.hpp>
#include <Script.hpp>

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
    configuration.MaxShots = 1000000000;
    configuration.SegmentsPerShot = 200;
    configuration.ShotsPerMerge = 300;
    configuration.MergesPerSegmentGC = 10;
}

void main(int argc, char* argv[])
{
    Printer printer;
    printer.ParseArgs(argc, argv);

    Configuration config;
    InitConfiguration(config);
    GlobalState gState = GlobalState(config, printer);

    Utils::MultiThread(config.TotalThreads, [&]()
        {
            //--- BEGIN BOILERPLATE ---
            
            //TODO: Maybe don't hardcode DLLs
            LPCWSTR dlls[4] = { L"sm64_jp_0.dll", L"sm64_jp_1.dll" , L"sm64_jp_2.dll" , L"sm64_jp_3.dll" };
            ThreadState tState = ThreadState(config, gState, omp_get_thread_num());
            Dll dll = Dll(dlls[tState.Id]);
            Script script(config, gState, tState, dll);

            SaveState state, state2;
            state.allocState(dll);
            state2.allocState(dll);
            Input* m64Diff = (Input*)malloc(sizeof(Input) * (config.SegmentLength * config.MaxSegments + 256)); // Todo: Nasty

            // Initialize game
            VOIDFUNC sm64_init = (VOIDFUNC)GetProcAddress(dll.hdll, "sm64_init");
            sm64_init();

            // Read inputs from file and advance to start frame
            Input* fileInputs = Utils::GetM64("C:\\Users\\Tyler\\Documents\\repos\\scattershot\\x64\\Debug\\4_units_from_edge.m64");
            script.AdvanceToStart(state, fileInputs);
            tState.LoadTime += state.riskyLoadJ(dll);

            // Initialize script
            script.Initialize(script.GetStateBin());

            //--- END BOILERPLATE ---

            for (int mainIteration = 0; mainIteration <= config.MaxShots; mainIteration++) {
                // ALWAYS START WITH A MERGE SO THE SHARED BLOCKS ARE OK.
                if (mainIteration % config.ShotsPerMerge == 0)
                    Utils::SingleThread([&]()
                        { 
                            gState.MergeState(mainIteration);
                            tState.PrintStatus(mainIteration);
                        });

                // Pick a block to "fire a scattershot" at
                if (!tState.SelectBaseBlock(mainIteration))
                    break;

                // Revert to initial state, and advance game state to end of block diff
                tState.LoadTime += state.riskyLoadJ(dll);
                tState.LightningLengthLocal = 0;
                tState.LightningLocal[tState.LightningLengthLocal++] = script.GetStateBin();
                int frameOffset = script.DecodeAndExecuteDiff(m64Diff);
                state2.save(dll);

                // Sanity check that state matches saved block state
                if (!tState.ValidateBaseBlock(script.GetStateBin()))
                    return;

                // "Fire" the scattershot, i.e. execute a batch of semi-random input sequences from the base block state.
                Input origLastIn = tState.CurrentInput;
                int origLightLenLocal = tState.LightningLengthLocal;
                for (int subLoop = 0; subLoop < config.SegmentsPerShot; subLoop++) {
                    tState.LoadTime += state2.riskyLoadJ(dll);

                    tState.CurrentInput = origLastIn;
                    tState.LightningLengthLocal = origLightLenLocal;

                    uint64_t baseRngSeed = tState.RngSeed;
                    int megaRandom = Utils::xoro_r(&tState.RngSeed) % 2;
                    script.ExtendTasFromBlock(m64Diff, frameOffset, megaRandom, baseRngSeed, tState.BaseBlock.pos);
                }
            }
        });
}
