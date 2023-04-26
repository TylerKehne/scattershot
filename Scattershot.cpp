#include <Scattershot.hpp>

//fifd: Given the data identifying the current block (pos), identify
//the index of that block in the hash table.
//Have not read this in detail or figured out the other arguments,
//but probably not important to understand
int Vec3d::findBlock(Block* blocks, int* hashTab, int maxHashes, int nMin, int nMax) {
    uint64_t tmpSeed = hashPos();
    for (int i = 0; i < 100; i++) {
        int inx = tmpSeed % maxHashes;
        int blockInx = hashTab[inx];
        if (blockInx == -1) return nMax;
        if (blockInx >= nMin && blockInx < nMax) {
            if (truncEq(blocks[blockInx].pos)) {
                return blockInx;
            }
        }
        Utils::xoro_r(&tmpSeed);
    }
    printf("Failed to find block from hash after 100 tries!\n");
    return -1; // TODO: Should be nMax?
}