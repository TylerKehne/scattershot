#include <Scattershot.hpp>

int Vec3d::findNewHashInx(int* hashTab, int maxHashes) {
    uint64_t tmpSeed = hashPos();
    for (int i = 0; i < 100; i++) {
        int inx = tmpSeed % maxHashes;
        if (hashTab[inx] == -1) return inx;
        Utils::xoro_r(&tmpSeed);
    }
    printf("Failed to find new hash index after 100 tries!\n");
    return -1;
}

uint64_t Vec3d::hashPos() {
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
int Vec3d::truncEq(Vec3d b) {
    return (x == b.x) && (y == b.y) && (z == b.z) && (s == b.s);
}

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

//UPDATED FOR SEGMENT STRUCT
int Block::blockLength() {
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
