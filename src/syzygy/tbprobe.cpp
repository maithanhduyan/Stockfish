/*
  Copyright (c) 2013 Ronald de Man
  This file may be redistributed and/or modified without restrictions.

  tbprobe.cpp contains the Stockfish-specific routines of the
  tablebase probing code. It should be relatively easy to adapt
  this code to other chess engines.
*/

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>   // For std::memset
#include <deque>
#include <list>
#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

#include "../bitboard.h"
#include "../movegen.h"
#include "../position.h"
#include "../search.h"
#include "../thread_win32.h"
#include "../types.h"

#include "tbprobe.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#define TBPIECES 6

using namespace Tablebases;

int Tablebases::MaxCardinality = 0;

namespace {

inline WDLScore operator-(WDLScore d) { return WDLScore(-int(d)); }
inline WDLScore operator+(WDLScore d1, WDLScore d2) { return WDLScore(int(d1) + int(d2)); }

inline Square operator^=(Square& s, int i) { return s = Square(int(s) ^ i); }
inline Square operator^(Square s, int i) { return Square(int(s) ^ i); }

struct PairsData {
    int blocksize;
    int idxbits;
    int num_indices;
    int real_num_blocks;
    int num_blocks;
    int max_len;
    int min_len;
    uint16_t* offset;
    uint8_t* sympat;
    uint8_t* indextable;
    uint16_t* sizetable;
    uint8_t* data;
    std::vector<uint64_t> base;
    std::vector<uint8_t> symlen;
    Piece pieces[TBPIECES];
    uint64_t factor[TBPIECES];
    uint8_t norm[TBPIECES];
};

// Helper struct to avoid manually define WDLEntry copy c'tor as we should
// because default one is not compatible with std::atomic_bool.
struct Atomic {
    Atomic() = default;
    Atomic(const Atomic& e) : ready(e.ready.load()) {}
    std::atomic_bool ready;
};

struct WDLEntry : Atomic {
    WDLEntry(const Position& pos, Key keys[]);
   ~WDLEntry();
    bool init(const std::string& fname);
    template<typename T> void do_init(T& e, uint8_t* data);

    void* baseAddress;
    uint64_t mapping;
    Key key;
    int pieceCount;
    bool symmetric;
    bool hasPawns;
    union {
        struct {
            typedef int Piece;

            bool hasUniquePieces;
            PairsData* precomp;
        } piece[2];

        struct {
            uint8_t pawnCount[2];
            struct {
                typedef int Pawn;

                PairsData* precomp;
            } file[2][4];
        } pawn;
    };
};

struct DTZEntry {

    enum Flag { STM = 1, Mapped = 2, WinPlies = 4, LossPlies = 8 };

    DTZEntry(const WDLEntry& wdl, Key wdlKeys[]);
   ~DTZEntry();
    bool init(const std::string& fname);
    template<typename T> void do_init(T& e, uint8_t* data);

    void* baseAddress;
    uint64_t mapping;
    Key key;
    Key key2;
    int pieceCount;
    bool symmetric;
    bool hasPawns;
    union {
        struct {
            typedef int Piece;

            bool hasUniquePieces;
            PairsData* precomp;
            uint8_t flags;
            uint16_t map_idx[4];
            uint8_t* map;
        } piece;

        struct {
            uint8_t pawnCount[2];
            struct {
                typedef int Pawn;

                PairsData* precomp;
                uint8_t flags;
                uint16_t map_idx[4];
            } file[4];
            uint8_t* map;
        } pawn;
    };
};

typedef decltype(WDLEntry::piece) WDLPiece;
typedef decltype(DTZEntry::piece) DTZPiece;
typedef decltype(WDLEntry::pawn ) WDLPawn;
typedef decltype(DTZEntry::pawn ) DTZPawn;

auto item(WDLPiece& e, int stm, int  ) -> decltype(e[stm])& { return e[stm]; }
auto item(DTZPiece& e, int    , int  ) -> decltype(e)& { return e; }
auto item(WDLPawn&  e, int stm, int f) -> decltype(e.file[stm][f])& { return e.file[stm][f]; }
auto item(DTZPawn&  e, int    , int f) -> decltype(e.file[f])& { return e.file[f]; }

const uint8_t Flap[] = {
    0,  0,  0,  0,  0,  0, 0,  0,
    0,  6, 12, 18, 18, 12, 6,  0,
    1,  7, 13, 19, 19, 13, 7,  1,
    2,  8, 14, 20, 20, 14, 8,  2,
    3,  9, 15, 21, 21, 15, 9,  3,
    4, 10, 16, 22, 22, 16, 10, 4,
    5, 11, 17, 23, 23, 17, 11, 5,
    0,  0,  0,  0,  0,  0,  0, 0
};

const uint8_t Ptwist[] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    47, 35, 23, 11, 10, 22, 34, 46,
    45, 33, 21,  9,  8, 20, 32, 44,
    43, 31, 19,  7,  6, 18, 30, 42,
    41, 29, 17,  5,  4, 16, 28, 40,
    39, 27, 15,  3,  2, 14, 26, 38,
    37, 25, 13,  1,  0, 12, 24, 36,
     0,  0,  0,  0,  0,  0,  0,  0
};

const uint8_t Invflap[] = {
     8, 16, 24, 32, 40, 48,
     9, 17, 25, 33, 41, 49,
    10, 18, 26, 34, 42, 50,
    11, 19, 27, 35, 43, 51
};

int MapB1H1H7[SQUARE_NB];
int MapA1D1D4[SQUARE_NB];
int KK_idx[10][SQUARE_NB]; // [MapA1D1D4][SQUARE_NB]

const uint8_t WDL_MAGIC[] = { 0x71, 0xE8, 0x23, 0x5D };
const uint8_t DTZ_MAGIC[] = { 0xD7, 0x66, 0x0C, 0xA5 };

const int wdl_to_dtz[] = { -1, -101, 0, 101, 1 };

const Value WDL_to_value[] = {
   -VALUE_MATE + MAX_PLY + 1,
    VALUE_DRAW - 2,
    VALUE_DRAW,
    VALUE_DRAW + 2,
    VALUE_MATE - MAX_PLY - 1
};

const std::string PieceToChar = " PNBRQK  pnbrqk";

Mutex TB_mutex;
std::string TBPaths;
std::deque<WDLEntry> WDLTable;
std::list<DTZEntry> DTZTable;

int Binomial[6][64];
int Pawnidx[5][24];
int Pfactor[5][4];

enum { BigEndian, LittleEndian };

template<typename T, int Half = sizeof(T)/2, int End = sizeof(T) - 1>
inline void swap_byte(T& x)
{
    char tmp, *c = (char*)(&x);
    if (Half) // Fix a MSVC 2015 warning
        for (int i = 0; i < Half; ++i)
            tmp = c[i], c[i] = c[End - i], c[End - i] = tmp;
}

template<typename T, int LE> T number(void* addr) {

    const union { uint32_t i; char c[4]; } Le = { 0x01020304 };
    const bool IsLittleEndian = (Le.c[0] == 4);

    T v = *((T*)addr);
    if (LE != IsLittleEndian)
        swap_byte(v);
    return v;
}

class HashTable {

    struct Entry {
        Key key;
        WDLEntry* ptr;
    };

    static const int TBHASHBITS = 10;
    static const int HSHMAX     = 5;

    Entry table[1 << TBHASHBITS][HSHMAX];

    void insert(Key key, WDLEntry* ptr) {
        Entry* entry = table[key >> (64 - TBHASHBITS)];

        for (int i = 0; i < HSHMAX; ++i, ++entry)
            if (!entry->ptr || entry->key == key) {
                entry->key = key;
                entry->ptr = ptr;
                return;
            }

        std::cerr << "HSHMAX too low!" << std::endl;
        exit(1);
    }

public:
  WDLEntry* operator[](Key key) {
      Entry* entry = table[key >> (64 - TBHASHBITS)];

      for (int i = 0; i < HSHMAX; ++i, ++entry)
          if (entry->key == key)
              return entry->ptr;

      return nullptr;
  }

  void clear() { std::memset(table, 0, sizeof(table)); }
  void insert(const std::vector<PieceType>& pieces);
};

HashTable WDLHash;


class TBFile : public std::ifstream {

    std::string fname;

public:
    // Open the file with the given name found among the TBPaths. TBPaths stores
    // the paths to directories where the .rtbw and .rtbz files can be found.
    // Multiple directories are separated by ";" on Windows and by ":" on
    // Unix-based operating systems.
    //
    // Example:
    // C:\tb\wdl345;C:\tb\wdl6;D:\tb\dtz345;D:\tb\dtz6
    TBFile(const std::string& f) {

#ifndef _WIN32
        const char SepChar = ':';
#else
        const char SepChar = ';';
#endif
        std::stringstream ss(TBPaths);
        std::string path;

        while (std::getline(ss, path, SepChar)) {
            fname = path + "/" + f;
            std::ifstream::open(fname);

            if (is_open())
                return;
        }
    }

    // Memory map the file and check it. File should be already open and
    // will be closed after mapping.
    uint8_t* map(void** baseAddress, uint64_t* mapping, const uint8_t TB_MAGIC[]) {

        if (!is_open()) {
            std::cerr << "Could not find " << fname << std::endl;
            *baseAddress = nullptr;
            return nullptr;
        }
        close();

#ifndef _WIN32
        struct stat statbuf;
        int fd = ::open(fname.c_str(), O_RDONLY);
        fstat(fd, &statbuf);
        *mapping = statbuf.st_size;
        *baseAddress = mmap(nullptr, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);

        if (*baseAddress == MAP_FAILED) {
            std::cerr << "Could not mmap() " << fname << std::endl;
            exit(1);
        }
#else
        HANDLE fd = CreateFile(fname.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD size_high;
        DWORD size_low = GetFileSize(fd, &size_high);
        HANDLE mmap = CreateFileMapping(fd, nullptr, PAGE_READONLY, size_high, size_low, nullptr);
        CloseHandle(fd);

        if (!mmap) {
            std::cerr << "CreateFileMapping() failed" << std::endl;
            exit(1);
        }

        *mapping = (uint64_t)mmap;
        *baseAddress = MapViewOfFile(mmap, FILE_MAP_READ, 0, 0, 0);

        if (!*baseAddress) {
            std::cerr << "MapViewOfFile() failed, name = " << fname
                      << ", error = " << GetLastError() << std::endl;
            exit(1);
        }
#endif
        uint8_t* data = (uint8_t*)*baseAddress;

        if (   *data++ != TB_MAGIC[0]
            || *data++ != TB_MAGIC[1]
            || *data++ != TB_MAGIC[2]
            || *data++ != TB_MAGIC[3]) {
            std::cerr << "Corrupted table in file " << fname << std::endl;
            unmap(*baseAddress, *mapping);
            *baseAddress = nullptr;
            return nullptr;
        }

        return data;
    }

    static void unmap(void* baseAddress, uint64_t mapping) {

#ifndef _WIN32
        munmap(baseAddress, mapping);
#else
        UnmapViewOfFile(baseAddress);
        CloseHandle((HANDLE)mapping);
#endif
    }
};

WDLEntry::WDLEntry(const Position& pos, Key keys[])
{
    memset(this, 0, sizeof(WDLEntry));

    key = keys[WHITE];
    pieceCount = pos.count<ALL_PIECES>(WHITE) + pos.count<ALL_PIECES>(BLACK);
    symmetric = (keys[WHITE] == keys[BLACK]);
    hasPawns = pos.pieces(PAWN);

    if (hasPawns) {
        // Set the leading color. In case both sides have pawns the leading color
        // is the side with less pawns because this leads to a better compression.
        bool c =   !pos.count<PAWN>(BLACK)
                || (   pos.count<PAWN>(WHITE)
                    && pos.count<PAWN>(BLACK) >= pos.count<PAWN>(WHITE));

        pawn.pawnCount[0] = pos.count<PAWN>(c ? WHITE : BLACK);
        pawn.pawnCount[1] = pos.count<PAWN>(c ? BLACK : WHITE);
    } else
        for (Color c = WHITE; c <= BLACK; ++c)
            for (PieceType pt = PAWN; pt < KING; ++pt)
                if (popcount(pos.pieces(c, pt)) == 1)
                    piece[0].hasUniquePieces = piece[1].hasUniquePieces = true;
}

WDLEntry::~WDLEntry()
{
    if (baseAddress)
        TBFile::unmap(baseAddress, mapping);

    if (hasPawns)
        for (File f = FILE_A; f <= FILE_D; ++f) {
            delete pawn.file[0][f].precomp;
            delete pawn.file[1][f].precomp;
        }
    else {
        delete piece[0].precomp;
        delete piece[1].precomp;
    }
}

DTZEntry::DTZEntry(const WDLEntry& wdl, Key wdlKeys[])
{
    memset(this, 0, sizeof(DTZEntry));

    key  = wdlKeys[0];
    key2 = wdlKeys[1];

    assert(key == wdl.key);

    pieceCount = wdl.pieceCount;
    symmetric = wdl.symmetric;
    hasPawns = wdl.hasPawns;

    if (hasPawns) {
        pawn.pawnCount[0] = wdl.pawn.pawnCount[0];
        pawn.pawnCount[1] = wdl.pawn.pawnCount[1];
    } else
        piece.hasUniquePieces = wdl.piece[0].hasUniquePieces;
}

DTZEntry::~DTZEntry()
{
    if (baseAddress)
        TBFile::unmap(baseAddress, mapping);

    if (hasPawns)
        for (File f = FILE_A; f <= FILE_D; ++f)
            delete pawn.file[f].precomp;
    else
        delete piece.precomp;
}

// Given a position with 6 or fewer pieces, produce a text string
// of the form KQPvKRP, where "KQP" represents the white pieces if
// mirror == false and the black pieces if mirror == true.
std::string file_name(const Position& pos, bool mirror)
{
    std::string w, b;

    for (PieceType pt = KING; pt >= PAWN; --pt) {
        w += std::string(popcount(pos.pieces(WHITE, pt)), PieceToChar[pt]);
        b += std::string(popcount(pos.pieces(BLACK, pt)), PieceToChar[pt]);
    }

    return mirror ? b + 'v' + w : w + 'v' + b;
}

void HashTable::insert(const std::vector<PieceType>& pieces)
{
    StateInfo st;
    Position pos;
    std::string code;

    for (PieceType pt : pieces)
        code += PieceToChar[pt];

    int bk = code.find('K', 1); // Black king
    TBFile f(code.substr(0, bk) + 'v' + code.substr(bk) + ".rtbw");

    if (!f.is_open())
        return;

    f.close();

    if (int(pieces.size()) > Tablebases::MaxCardinality)
        Tablebases::MaxCardinality = pieces.size();

    Key keys[] = { pos.set(code, WHITE, &st).material_key(),
                   pos.set(code, BLACK, &st).material_key() };

    WDLTable.push_back(WDLEntry(pos.set(code, WHITE, &st), keys));

    insert(keys[WHITE], &WDLTable.back());
    insert(keys[BLACK], &WDLTable.back());
}

int decompress_pairs(PairsData* d, uint64_t idx)
{
    if (!d->idxbits)
        return d->min_len;

    // idx = blockidx | litidx where litidx is a signed number of lenght d->idxbits
    uint32_t blockidx = (uint32_t)(idx >> d->idxbits);
    int litidx = (idx & ((1ULL << d->idxbits) - 1)) - (1ULL << (d->idxbits - 1));

    // indextable points to an array of blocks of 6 bytes representing numbers in
    // little endian. The low 4 bytes are the block, the high 2 bytes the idxOffset.
    uint32_t block = number<uint32_t, LittleEndian>(d->indextable + 6 * blockidx);
    litidx += number<uint16_t, LittleEndian>(d->indextable + 6 * blockidx + 4);

    while (litidx < 0)
        litidx += d->sizetable[--block] + 1;

    while (litidx > d->sizetable[block])
        litidx -= d->sizetable[block++] + 1;

    uint32_t* ptr = (uint32_t*)(d->data + (block << d->blocksize));
    uint64_t code = number<uint64_t, BigEndian>(ptr);

    int m = d->min_len;
    uint16_t *offset = d->offset;
    int sym, bitcnt;

    ptr += 2;
    bitcnt = 0; // number of "empty bits" in code

    for (;;) {
        int l = m;

        while (code < d->base[l - d->min_len])
            ++l;

        sym = number<uint16_t, LittleEndian>(offset + l);
        sym += (int)((code - d->base[l - d->min_len]) >> (64 - l));

        if (litidx < (int)d->symlen[sym] + 1)
            break;

        litidx -= (int)d->symlen[sym] + 1;
        code <<= l;
        bitcnt += l;

        if (bitcnt >= 32) {
            bitcnt -= 32;
            code |= (uint64_t)number<uint32_t, BigEndian>(ptr++) << bitcnt;
        }
    }

    uint8_t *sympat = d->sympat;

    while (d->symlen[sym] != 0) {
        uint8_t* w = sympat + (3 * sym);
        int s1 = ((w[1] & 0xf) << 8) | w[0];

        if (litidx < (int)d->symlen[s1] + 1)
            sym = s1;
        else {
            litidx -= (int)d->symlen[s1] + 1;
            sym = (w[2] << 4) | (w[1] >> 4);
        }
    }

    return sympat[3 * sym];
}

template<typename Entry>
bool check_dtz_stm(Entry*, File, int) { return true; }

template<>
bool check_dtz_stm(DTZEntry* entry, File f, int stm) {

    uint8_t flags = entry->hasPawns ? entry->pawn.file[f].flags
                                    : entry->piece.flags;

    return   (flags & DTZEntry::Flag::STM) == stm
          || (entry->symmetric && !entry->hasPawns);
}

// DTZ scores are sorted by frequency of occurrence and then assigned the
// values 0, 1, 2, 3, ... in order of decreasing frequency. This is done
// in each of the four ranges. The mapping information necessary to
// reconstruct the original values is stored in the TB file and used to
// initialise the map[] array during initialisation of the TB.
template<typename Entry>
int map_score(Entry*, File, int value, WDLScore) { return value - 2; }

template<>
int map_score(DTZEntry* entry, File f, int value, WDLScore wdl) {

    const int WDLMap[]  = { 1, 3, 0, 2, 0 };

    uint8_t flags = entry->hasPawns ? entry->pawn.file[f].flags
                                    : entry->piece.flags;

    uint8_t* map  = entry->hasPawns ? entry->pawn.map
                                    : entry->piece.map;

    uint16_t* idx = entry->hasPawns ? entry->pawn.file[f].map_idx
                                    : entry->piece.map_idx;
    if (flags & DTZEntry::Flag::Mapped)
        value = map[idx[WDLMap[wdl + 2]] + value];

    // DTZ tables store distance to zero in number of moves but
    // under some conditions we want to return plies, so we have
    // to multiply score by 2.
    if (   (wdl == WDLWin  && !(flags & DTZEntry::Flag::WinPlies))
        || (wdl == WDLLoss && !(flags & DTZEntry::Flag::LossPlies))
        ||  wdl == WDLCursedWin
        ||  wdl == WDLCursedLoss)
        value *= 2;

    return value;
}

int off_A1H8(Square sq) { return int(rank_of(sq)) - file_of(sq); }

template<typename Entry>
uint64_t probe_table(const Position& pos,  Entry* entry, WDLScore wdl = WDLDraw, int* success = nullptr)
{
    Square squares[TBPIECES];
    Piece pieces[TBPIECES];
    uint64_t idx;
    int stm, next = 0, flipColor = 0, flipSquares = 0, size = 0, leadPawnsCnt = 0;
    bool hasUniquePieces;
    PairsData* precomp;
    Bitboard b, leadPawns = 0;
    File tbFile = FILE_A;

    // A given TB entry like KRK has associated two material keys: KRvk and Kvkr.
    // If both sides have the same pieces we have a symmetric material and the
    // keys are equal. The stored TB entry is calculated always with WHITE side
    // to move and if the position to lookup has instead BLACK to move, we need
    // to switch color and flip the squares before the lookup:
    if (entry->symmetric) {
        flipColor = pos.side_to_move() * 8;     // Switch color
        flipSquares = pos.side_to_move() * 070; // Vertical flip: SQ_A8 -> SQ_A1
        stm = WHITE;
    }
    // In case of sides with different pieces, if the position to look up has a
    // different key form the stored one (entry->key), then we have to switch
    // color and flip the squares:
    else {
        flipColor   = (pos.material_key() != entry->key) * 8;
        flipSquares = (pos.material_key() != entry->key) * 070;

        // TB entry is stored with WHITE as stronger side, so side to move has
        // to be flipped accordingly, for example Kvkr (white to move) maps to
        // KRvk (black to move).
        stm = (pos.material_key() != entry->key) ^ pos.side_to_move();
    }

    // For pawns, TB files store separate tables according if leading pawn is on
    // file a, b, c or d after reordering. To determine which of the 4 tables
    // must be probed we need to extract the position's leading pawns then order
    // them according to Flap table, in ascending order and finally pick the file
    // of the pawn with minimum Flap[]. The new pawn order should be preserved
    // because needed for next steps.
    if (entry->hasPawns) {
        Piece pc = Piece(item(entry->pawn, 0, 0).precomp->pieces[0] ^ flipColor);

        assert(type_of(pc) == PAWN);

        leadPawns = b = pos.pieces(color_of(pc), PAWN);
        while (b)
            squares[size++] = pop_lsb(&b) ^ flipSquares;

        leadPawnsCnt = size;

        auto flap = [] (Square i, Square j) { return Flap[i] < Flap[j]; };
        std::sort(squares, squares + size, flap);

        tbFile = file_of(squares[0]);
        if (tbFile > FILE_D)
            tbFile = file_of(squares[0] ^ 7); // Horizontal flip: SQ_H1 -> SQ_A1

        precomp = item(entry->pawn, stm, tbFile).precomp;
    } else
        precomp = item(entry->piece, stm, 0).precomp;

    // DTZ tables are one-sided, i.e. they store positions only for white to
    // move or only for black to move, so check for side to move to be stm,
    // early exit otherwise.
    if (    std::is_same<Entry, DTZEntry>::value
        && !check_dtz_stm(entry, tbFile, stm)) {
        *success = -1;
        return 0;
    }

    // Now we are ready to get all the position pieces (but the lead pawns) and
    // directly map them to the correct color and square.
    b = pos.pieces() ^ leadPawns;
    for ( ; b; ++size) {
        Square sq = pop_lsb(&b);
        squares[size] = sq ^ flipSquares;
        pieces[size] = Piece(pos.piece_on(sq) ^ flipColor);
    }

    // Then we reorder the pieces to have the same sequence as the one stored
    // in precomp->pieces[i], this is important for the next step. The sequence
    // stored is the one that ensures the best compression.
    for (int i = leadPawnsCnt; i < size; ++i)
        for (int j = i; j < size; ++j)
            if (precomp->pieces[i] == pieces[j])
            {
                std::swap(pieces[i], pieces[j]);
                std::swap(squares[i], squares[j]);
                break;
            }

    // Now we map again the squares so that the square of the lead piece is in
    // the triangle A1-D1-D4. We take care that the condition on the diagonal
    // flip is checked after horizontal and vertical flips are already done.
    if (file_of(squares[0]) > FILE_D)
        for (int i = 0; i < size; ++i)
            squares[i] ^= 7; // Horizontal flip: SQ_H1 -> SQ_A1

    // Reorder the leading pawns according to Ptwist table, in descending order,
    // and encode them.
    if (entry->hasPawns) {

        auto ptwist = [] (Square i, Square j) { return Ptwist[i] > Ptwist[j]; };
        std::sort(squares + 1, squares + leadPawnsCnt, ptwist);

        idx = Pawnidx[leadPawnsCnt - 1][Flap[squares[0]]];

        for (int i = 1; i < leadPawnsCnt; ++i)
            idx += Binomial[i][Ptwist[squares[i]]];

        next = leadPawnsCnt;
        goto encode_remaining; // With pawns we have finished special treatments
    }

    if (rank_of(squares[0]) > RANK_4)
        for (int i = 0; i < size; ++i)
            squares[i] ^= 070; // Vertical flip: SQ_A8 -> SQ_A1

    // Look for the first piece not on the A1-D4 diagonal and ensure it is
    // mapped below the diagonal.
    hasUniquePieces = item(entry->piece, stm, 0).hasUniquePieces;

    for (int i = 0; i < size; ++i) {
        if (!off_A1H8(squares[i]))
            continue;

        if (off_A1H8(squares[i]) > 0 && i < (hasUniquePieces ? 3 : 2))
            for (int j = i; j < size; ++j) // A1-H8 diagonal flip: SQ_A3 -> SQ_C3
                squares[j] = Square(((squares[j] >> 3) | (squares[j] << 3)) & 63);
        break;
    }

    // The encoding function maps a position to its index into the table.
    // Suppose we have KRvK. Let's say the pieces are on square numbers wK, wR
    // and bK (each 0...63). The simplest way to map this position to an index
    // is like this:
    //
    //   index = wK * 64*64 + wR * 64 + bK;
    //
    // But this way the TB is going to have 64*64*64 = 262144 positions, with
    // lots of positions being equivalent (because they are mirrors of each
    // other) and lots of positions being invalid (two pieces on one square,
    // adjacent kings, etc.).
    // Usually the first step is to take the wK and bK together. There are just
    // 462 ways legal and not-mirrored ways to place the wK and bK on the board.
    // Once we have placed the wK and bK, there are 62 squares left for the wR
    // Mapping its square from 0..63 to 0..61 can be done like:
    //
    //   wR -= (wR > wK) + (wR > bK);
    //
    // In words: if wR "comes later" than wK, we deduct 1, and the same if wR
    // "comes later" than bK. In case of two same pieces like KRRvK we want to
    // place the two Rs "together". If we have 62 squares left, we can place two
    // Rs "together" in 62*61/2 ways.

    // In case we have at least 3 unique pieces (inlcuded kings) we encode them
    // together.
    if (hasUniquePieces) {

        int adjust1 =  squares[1] > squares[0];
        int adjust2 = (squares[2] > squares[0]) + (squares[2] > squares[1]);

        // MapA1D1D4[] maps the b1-d1-d3 triangle to 0...5. There are 63 squares
        // for second piece and and 62 (mapped to 0...61) for the third.
        if (off_A1H8(squares[0]))
            idx =   MapA1D1D4[squares[0]] * 63 * 62
                 + (squares[1] - adjust1) * 62
                 +  squares[2] - adjust2;

        // First piece is on diagonal: map to 6, rank_of() maps a1-d4 diagonal
        // to 0...3 and MapB1H1H7[] maps the b1-h1-h7 triangle to 0..27
        else if (off_A1H8(squares[1]))
            idx =                      6 * 63 * 62
                 + rank_of(squares[0])   * 28 * 62
                 + MapB1H1H7[squares[1]] * 62
                 + squares[2] - adjust2;

        // First 2 pieces are on the diagonal a1-h8
        else if (off_A1H8(squares[2]))
            idx =  6 * 63 * 62 + 4 * 28 * 62
                 +  rank_of(squares[0])        * 7 * 28
                 + (rank_of(squares[1]) - adjust1) * 28
                 +  MapB1H1H7[squares[2]];

        // All 3 pieces on the diagonal a1-h8
        else
            idx = 6 * 63 * 62 + 4 * 28 * 62 + 4 * 7 * 28
                 +  rank_of(squares[0])         * 7 * 6
                 + (rank_of(squares[1]) - adjust1)  * 6
                 + (rank_of(squares[2]) - adjust2);

        next = 3; // Continue encoding form piece[3]
    } else {
        // We don't have at least 3 unique pieces, like in KRRvKBB, just map
        // the kings and set next to 2.
        idx = KK_idx[MapA1D1D4[squares[0]]][squares[1]];
        next = 2;
    }

encode_remaining:
    idx *= precomp->factor[0];

    // Reorder remainig pawns then pieces according to square, in ascending order
    int remainingPawns = entry->hasPawns ? entry->pawn.pawnCount[1] : 0;

    while (next < size) {

        int end = next + (remainingPawns ? remainingPawns : precomp->norm[next]);

        std::sort(squares + next, squares + end);

        uint64_t s = 0;

        // Map squares to lower index if "come later" than previous (as done earlier for pieces)
        for (int i = next; i < end; ++i) {
            int adjust = 0;

            for (int j = 0; j < next; ++j)
                adjust += squares[i] > squares[j];

            s += Binomial[i - next + 1][squares[i] - adjust - (remainingPawns ? 8 : 0)];
        }

        remainingPawns = 0;
        idx += s * precomp->factor[next];
        next = end;
    }

    // Now that we have the index, decompress the pair and get the score
    return map_score(entry, tbFile, decompress_pairs(precomp, idx), wdl);
}

template<typename T>
int get_pfactor(const T& p, File, typename T::Piece = 0)
{ return p.hasUniquePieces ? 31332 : 462; }

template<typename T>
int get_pfactor(const T& p, File f, typename T::Pawn = 0)
{ return Pfactor[p.precomp->norm[0] - 1][f]; }


template<typename T>
uint64_t set_factors(T& p, int num, int order[], File f)
{
    PairsData* d = p.precomp;
    int i = d->norm[0];

    if (order[1] < 0xF)
        i += d->norm[i];

    int n = 64 - i;
    uint64_t size = 1;

    for (int k = 0; i < num || k == order[0] || k == order[1]; ++k)
        if (k == order[0]) {
            d->factor[0] = size;
            size *= get_pfactor(p, f);
        } else if (k == order[1]) {
            d->factor[d->norm[0]] = size;
            size *= Binomial[d->norm[d->norm[0]]][48 - d->norm[0]];
        } else {
            d->factor[i] = size;
            size *= Binomial[d->norm[i]][n];
            n -= d->norm[i];
            i += d->norm[i];
        }

    return size;
}

template<typename T>
void set_norms(T* p, int num, const uint8_t pawns[])
{
    p->norm[0] = pawns[0];

    if (pawns[1])
        p->norm[pawns[0]] = pawns[1];

    for (int i = pawns[0] + pawns[1]; i < num; i += p->norm[i])
        for (int j = i; j < num && p->pieces[j] == p->pieces[i]; ++j)
            ++p->norm[i];
}

void calc_symlen(PairsData* d, size_t s, std::vector<uint8_t>& tmp)
{
    int s1, s2;

    uint8_t* w = d->sympat + 3 * s;
    s2 = (w[2] << 4) | (w[1] >> 4);

    if (s2 == 0xFFF)
        d->symlen[s] = 0;
    else {
        s1 = ((w[1] & 0xF) << 8) | w[0];

        if (!tmp[s1])
            calc_symlen(d, s1, tmp);

        if (!tmp[s2])
            calc_symlen(d, s2, tmp);

        d->symlen[s] = d->symlen[s1] + d->symlen[s2] + 1;
    }

    tmp[s] = 1;
}

uint8_t* set_sizes(PairsData* d, uint8_t* data, uint64_t tb_size)
{
    if (*data++ & 0x80) {
        d->min_len = *data++;
        return data;
    }

    d->blocksize = *data++;
    d->idxbits = *data++;
    d->num_indices = (tb_size + (1ULL << d->idxbits) - 1) >> d->idxbits; // Divide and round upward, like ceil()
    d->num_blocks = number<uint8_t, LittleEndian>(data++);
    d->real_num_blocks = number<uint32_t, LittleEndian>(data); data += sizeof(uint32_t);
    d->num_blocks += d->real_num_blocks;
    d->max_len = *data++;
    d->min_len = *data++;
    d->offset = (uint16_t*)data;
    d->base.resize(d->max_len - d->min_len + 1);

    for (int i = d->base.size() - 2; i >= 0; --i)
        d->base[i] = (d->base[i + 1] + number<uint16_t, LittleEndian>(d->offset + i)
                                     - number<uint16_t, LittleEndian>(d->offset + i + 1)) / 2;

    for (size_t i = 0; i < d->base.size(); ++i)
        d->base[i] <<= (64 - d->min_len) - i;

    d->offset -= d->min_len;

    data += d->base.size() * sizeof(*d->offset);
    d->symlen.resize(number<uint16_t, LittleEndian>(data)); data += sizeof(uint16_t);
    d->sympat = data;

    std::vector<uint8_t> tmp(d->symlen.size());

    for (size_t i = 0; i < d->symlen.size(); ++i)
        if (!tmp[i])
            calc_symlen(d, i, tmp);

    return data + 3 * d->symlen.size() + (d->symlen.size() & 1);
}

template<typename T>
void WDLEntry::do_init(T& e, uint8_t* data)
{
    PairsData* d;
    uint64_t tb_size[8];

    enum { Split = 1, HasPawns = 2 };

    uint8_t flags = *data++;

    int split    = (flags & Split);
    File maxFile = (flags & HasPawns) ? FILE_D : FILE_A;

    assert(hasPawns  == !!(flags & HasPawns));
    assert(symmetric != !!(flags & Split));

    bool pp = (flags & HasPawns) && pawn.pawnCount[1]; // Pawns on both sides

    assert(!pp || pawn.pawnCount[0]);

    for (File f = FILE_A; f <= maxFile; ++f) {

        for (int k = 0; k < 2; k++)
            item(e, k, f).precomp = new PairsData();

        int order[][2] = { { *data & 0xF, pp ? *(data + 1) & 0xF : 0xF },
                           { *data >>  4, pp ? *(data + 1) >>  4 : 0xF } };
        data += 1 + pp;

        for (int i = 0; i < pieceCount; ++i, ++data) {
            item(e, 0, f).precomp->pieces[i] = Piece(*data & 0xF);
            item(e, 1, f).precomp->pieces[i] = Piece(*data >>  4);
        }

        uint8_t pn[] = { uint8_t(piece[0].hasUniquePieces ? 3 : 2), 0 };

        for (int i = 0; i < 2; ++i) {
            set_norms(item(e, i, f).precomp, pieceCount, (flags & HasPawns) ? pawn.pawnCount : pn);
            tb_size[2 * f + i] = set_factors(item(e, i, f), pieceCount, order[i], f);
        }
    }

    data += (uintptr_t)data & 1; // Word alignment

    for (File f = FILE_A; f <= maxFile; ++f)
        for (int k = 0; k <= split; k++)
            data = set_sizes(item(e, k, f).precomp, data, tb_size[2 * f + k]);

    for (File f = FILE_A; f <= maxFile; ++f)
        for (int k = 0; k <= split; k++) {
            (d = item(e, k, f).precomp)->indextable = data;
            data += 6ULL * d->num_indices;
        }

    for (File f = FILE_A; f <= maxFile; ++f)
        for (int k = 0; k <= split; k++) {
            (d = item(e, k, f).precomp)->sizetable = (uint16_t*)data;
            data += 2ULL * d->num_blocks;
        }

    for (File f = FILE_A; f <= maxFile; ++f)
        for (int k = 0; k <= split; k++) {
            data = (uint8_t*)(((uintptr_t)data + 0x3F) & ~0x3F); // 64 byte alignment
            (d = item(e, k, f).precomp)->data = data;
            data += (1ULL << d->blocksize) * d->real_num_blocks;
        }
}

bool WDLEntry::init(const std::string& fname)
{
    uint8_t* data = TBFile(fname).map(&baseAddress, &mapping, WDL_MAGIC);
    if (!data)
        return false;

    hasPawns ? do_init(pawn, data) : do_init(piece, data);
    return true;
}

template<typename T>
void DTZEntry::do_init(T& e, uint8_t* data)
{
    PairsData* d;
    uint64_t tb_size[8];

    enum { Split = 1, HasPawns = 2 };

    uint8_t flags = *data++;

    File maxFile = (flags & HasPawns) ? FILE_D : FILE_A;

    assert(hasPawns  == !!(flags & HasPawns));
    assert(symmetric != !!(flags & Split));

    bool pp = (flags & HasPawns) && pawn.pawnCount[1]; // Pawns on both sides

    assert(!pp || pawn.pawnCount[0]);

    for (File f = FILE_A; f <= maxFile; ++f) {

        item(e, 0, f).precomp = new PairsData();

        int order[][2] = { { *data & 0xF, pp ? *(data + 1) & 0xF : 0xF },
                           { *data >>  4, pp ? *(data + 1) >>  4 : 0xF } };
        data += 1 + pp;

        for (int i = 0; i < pieceCount; ++i, ++data)
            item(e, 0, f).precomp->pieces[i] = Piece(*data & 0xF);

        uint8_t pn[] = { uint8_t(piece.hasUniquePieces ? 3 : 2), 0 };

        set_norms(item(e, 0, f).precomp, pieceCount, (flags & HasPawns) ? pawn.pawnCount : pn);
        tb_size[f] = set_factors(item(e, 0, f), pieceCount, order[0], f);
    }

    data += (uintptr_t)data & 1; // Word alignment

    for (File f = FILE_A; f <= maxFile; ++f) {
        assert(!(*data & 0x80));

        item(e, 0, f).flags = *data;
        data = set_sizes(item(e, 0, f).precomp, data, tb_size[f]);
    }

    e.map = data;

    for (File f = FILE_A; f <= maxFile; ++f) {
        if (item(e, 0, f).flags & 2)
            for (int i = 0; i < 4; ++i) { // Sequence like 3,x,x,x,1,x,0,2,x,x
                item(e, 0, f).map_idx[i] = (uint16_t)(data - e.map + 1);
                data += *data + 1;
            }
    }

    data += (uintptr_t)data & 1;

    for (File f = FILE_A; f <= maxFile; ++f) {
        (d = item(e, 0, f).precomp)->indextable = data;
        data += 6ULL * d->num_indices;
    }

    for (File f = FILE_A; f <= maxFile; ++f) {
        (d = item(e, 0, f).precomp)->sizetable = (uint16_t*)data;
        data += 2ULL * d->num_blocks;
    }

    for (File f = FILE_A; f <= maxFile; ++f) {
        data = (uint8_t*)(((uintptr_t)data + 0x3F) & ~0x3F); // 64 byte alignment
        (d = item(e, 0, f).precomp)->data = data;
        data += (1ULL << d->blocksize) * d->real_num_blocks;
    }
}

bool DTZEntry::init(const std::string& fname)
{
    uint8_t* data = TBFile(fname).map(&baseAddress, &mapping, DTZ_MAGIC);
    if (!data)
        return false;

    hasPawns ? do_init(pawn, data) : do_init(piece, data);
    return true;
}

WDLScore probe_wdl_table(Position& pos, int* success)
{
    Key key = pos.material_key();

    if (!(pos.pieces() ^ pos.pieces(KING)))
        return WDLDraw; // KvK

    WDLEntry* entry = WDLHash[key];
    if (!entry) {
        *success = 0;
        return WDLDraw;
    }

    // Init table at first access attempt. Special care to avoid
    // one thread reads ready == 1 while the other is still in
    // init(), this could happen due to compiler reordering.
    if (!entry->ready.load(std::memory_order_acquire)) {
        std::unique_lock<Mutex> lk(TB_mutex);
        if (!entry->ready.load(std::memory_order_relaxed)) {
            std::string fname = file_name(pos, entry->key != key) + ".rtbw";
            if (!entry->init(fname)) {
                // Was ptr2->key = 0ULL;  Just leave !ptr->ready condition
                *success = 0;
                return WDLDraw;
            }
            entry->ready.store(1, std::memory_order_release);
        }
    }

    assert(key == entry->key || !entry->symmetric);

    return (WDLScore)probe_table(pos, entry);
}

int probe_dtz_table(const Position& pos, WDLScore wdl, int *success)
{
    Key key = pos.material_key();

    if (DTZTable.front().key != key && DTZTable.front().key2 != key) {

        // Enforce "Most Recently Used" (MRU) order for DTZ list
        for (auto it = DTZTable.begin(); it != DTZTable.end(); ++it)
            if (it->key == key || it->key2 == key) {
                // Move to front without deleting the element
                DTZTable.splice(DTZTable.begin(), DTZTable, it);
                break;
            }

        // If still not found, add a new one
        if (DTZTable.front().key != key && DTZTable.front().key2 != key) {

            WDLEntry* wdlEntry = WDLHash[key];
            if (!wdlEntry) {
                *success = 0;
                return 0;
            }

            StateInfo st;
            Position p;
            std::string wdlCode = file_name(pos, wdlEntry->key != key);
            std::string fname = wdlCode + ".rtbz";
            wdlCode.erase(wdlCode.find('v'), 1);

            Key wdlKeys[] = { p.set(wdlCode, WHITE, &st).material_key(),
                              p.set(wdlCode, BLACK, &st).material_key() };

            DTZTable.push_front(DTZEntry(*wdlEntry, wdlKeys));

            if (!DTZTable.front().init(fname)) {
                // In case file is not found init() fails, but we leave
                // the entry so to avoid rechecking at every probe (same
                // functionality as WDL case).
                // FIXME: This is different form original functionality!
                /* DTZTable.pop_front(); */
                *success = 0;
                return 0;
            }

            // Keep list size within 64 entries
            // FIXME remove it when we will know what we are doing
            if (DTZTable.size() > 64)
               DTZTable.pop_back();
        }
    }

    if (!DTZTable.front().baseAddress) {
        *success = 0;
        return 0;
    }

    return probe_table(pos, &DTZTable.front(), wdl, success);
}

// Add underpromotion captures to list of captures.
ExtMove *add_underprom_caps(Position& pos, ExtMove *stack, ExtMove *end)
{
    ExtMove *moves, *extra = end;

    for (moves = stack; moves < end; ++moves) {
        Move move = moves->move;

        if (type_of(move) == PROMOTION && !pos.empty(to_sq(move))) {
            (*extra++).move = (Move)(move - (1 << 12));
            (*extra++).move = (Move)(move - (2 << 12));
            (*extra++).move = (Move)(move - (3 << 12));
        }
    }

    return extra;
}

WDLScore probe_ab(Position& pos, WDLScore alpha, WDLScore beta, int *success)
{
    WDLScore value;
    ExtMove stack[64];
    ExtMove *moves, *end;
    StateInfo st;

    // Generate (at least) all legal non-ep captures including (under)promotions.
    // It is OK to generate more, as long as they are filtered out below.
    if (!pos.checkers()) {
        end = generate<CAPTURES>(pos, stack);
        // Since underpromotion captures are not included, we need to add them.
        end = add_underprom_caps(pos, stack, end);
    } else
        end = generate<EVASIONS>(pos, stack);

    CheckInfo ci(pos);

    for (moves = stack; moves < end; ++moves) {
        Move capture = moves->move;

        if (   !pos.capture(capture)
            ||  type_of(capture) == ENPASSANT
            || !pos.legal(capture, ci.pinned))
            continue;

        pos.do_move(capture, st, pos.gives_check(capture, ci));
        value = -probe_ab(pos, -beta, -alpha, success);
        pos.undo_move(capture);

        if (*success == 0)
            return WDLDraw;

        if (value > alpha) {
            if (value >= beta) {
                *success = 2;
                return value;
            }

            alpha = value;
        }
    }

    value = probe_wdl_table(pos, success); // FIXME why this is not at the beginning?

    if (*success == 0)
        return WDLDraw;

    if (alpha >= value) {
        *success = 1 + (alpha > 0);
        return alpha;
    } else {
        *success = 1;
        return value;
    }
}

int probe_dtz(Position& pos, int *success);

// This routine treats a position with en passant captures as one without.
int probe_dtz_no_ep(Position& pos, int *success)
{
    int dtz;

    WDLScore wdl = probe_ab(pos, WDLLoss, WDLWin, success);

    if (!*success)
        return 0;

    if (wdl == WDLDraw)
        return 0;

    if (*success == 2)
        return wdl == WDLWin ? 1 : 101;

    ExtMove stack[MAX_MOVES];
    ExtMove *moves, *end = nullptr;
    StateInfo st;
    CheckInfo ci(pos);

    if (wdl > 0) {
        // Generate at least all legal non-capturing pawn moves
        // including non-capturing promotions.
        if (!pos.checkers())
            end = generate<NON_EVASIONS>(pos, stack);
        else
            end = generate<EVASIONS>(pos, stack);

        for (moves = stack; moves < end; ++moves) {
            Move move = moves->move;

            if (   type_of(pos.moved_piece(move)) != PAWN
                || pos.capture(move)
                || !pos.legal(move, ci.pinned))
                continue;

            pos.do_move(move, st, pos.gives_check(move, ci));
            WDLScore v = -probe_ab(pos, WDLLoss, -wdl + WDLCursedWin, success);
            pos.undo_move(move);

            if (*success == 0) return 0;

            if (v == wdl)
                return v == WDLWin ? 1 : 101;
        }
    }

    dtz = 1 + probe_dtz_table(pos, wdl, success);

    if (*success >= 0) {
        if (wdl & 1) dtz += 100;

        return wdl >= 0 ? dtz : -dtz;
    }

    if (wdl > 0) {
        int best = 0xffff;

        for (moves = stack; moves < end; ++moves) {
            Move move = moves->move;

            if (pos.capture(move) || type_of(pos.moved_piece(move)) == PAWN
                    || !pos.legal(move, ci.pinned))
                continue;

            pos.do_move(move, st, pos.gives_check(move, ci));
            int v = -probe_dtz(pos, success);
            pos.undo_move(move);

            if (*success == 0)
                return 0;

            if (v > 0 && v + 1 < best)
                best = v + 1;
        }

        return best;
    } else {
        int best = -1;

        if (!pos.checkers())
            end = generate<NON_EVASIONS>(pos, stack);
        else
            end = generate<EVASIONS>(pos, stack);

        for (moves = stack; moves < end; ++moves) {
            int v;
            Move move = moves->move;

            if (!pos.legal(move, ci.pinned))
                continue;

            pos.do_move(move, st, pos.gives_check(move, ci));

            if (st.rule50 == 0) {
                if (wdl == -2) v = -1;
                else {
                    v = probe_ab(pos, WDLCursedWin, WDLWin, success);
                    v = (v == 2) ? 0 : -101;
                }
            } else {
                v = -probe_dtz(pos, success) - 1;
            }

            pos.undo_move(move);

            if (*success == 0)
                return 0;

            if (v < best)
                best = v;
        }

        return best;
    }
}

// Probe the DTZ table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This implies that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
//
int probe_dtz(Position& pos, int *success)
{
    *success = 1;
    int v = probe_dtz_no_ep(pos, success);

    if (pos.ep_square() == SQ_NONE)
        return v;

    if (*success == 0)
        return 0;

    // Now handle en passant.
    int v1 = -3;

    ExtMove stack[MAX_MOVES];
    ExtMove *moves, *end;
    StateInfo st;

    if (!pos.checkers())
        end = generate<CAPTURES>(pos, stack);
    else
        end = generate<EVASIONS>(pos, stack);

    CheckInfo ci(pos);

    for (moves = stack; moves < end; ++moves) {
        Move capture = moves->move;

        if (type_of(capture) != ENPASSANT
                || !pos.legal(capture, ci.pinned))
            continue;

        pos.do_move(capture, st, pos.gives_check(capture, ci));
        WDLScore v0 = -probe_ab(pos, WDLLoss, WDLWin, success);
        pos.undo_move(capture);

        if (*success == 0)
            return 0;

        if (v0 > v1) v1 = v0;
    }

    if (v1 > -3) {
        v1 = wdl_to_dtz[v1 + 2];

        if (v < -100) {
            if (v1 >= 0)
                v = v1;
        } else if (v < 0) {
            if (v1 >= 0 || v1 < -100)
                v = v1;
        } else if (v > 100) {
            if (v1 > 0)
                v = v1;
        } else if (v > 0) {
            if (v1 == 1)
                v = v1;
        } else if (v1 >= 0) {
            v = v1;
        } else {
            for (moves = stack; moves < end; ++moves) {
                Move move = moves->move;

                if (type_of(move) == ENPASSANT) continue;

                if (pos.legal(move, ci.pinned))
                    break;
            }

            if (moves == end && !pos.checkers()) {
                end = generate<QUIETS>(pos, end);

                for (; moves < end; ++moves) {
                    Move move = moves->move;

                    if (pos.legal(move, ci.pinned))
                        break;
                }
            }

            if (moves == end)
                v = v1;
        }
    }

    return v;
}

} // namespace

void Tablebases::init(const std::string& paths)
{
    DTZTable.clear();
    WDLTable.clear();
    WDLHash.clear();

    MaxCardinality = 0;
    TBPaths = paths;

    if (TBPaths.empty() || TBPaths == "<empty>")
        return;

    // Fill binomial[] with the Binomial Coefficents using Pascal triangle
    Binomial[0][0] = 1;

    for (int n = 1; n < 64; n++)
        for (int k = 0; k < 6 && k <= n; ++k)
            Binomial[k][n] = (k > 0 ? Binomial[k-1][n-1] : 0)
                           + (k < n ? Binomial[k][n-1] : 0);

    for (int i = 0; i < 5; ++i) {
        int k = 0;

        for (int j = 1; j <= 4; ++j) {
            int s = 0;

            for ( ; k < 6 * j; ++k) {
                Pawnidx[i][k] = s;
                s += Binomial[i][Ptwist[Invflap[k]]];
            }

            Pfactor[i][j - 1] = s;
        }
    }

    // Compute MapB1H1H7[] that encodes a square below a1-h8 diagonal to 0..27
    int code = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        if (off_A1H8(s) < 0)
            MapB1H1H7[s] = code++;

    // Compute MapA1D1D4[] that encodes a square on the a1-d1-d4 triangle to 0..9
    std::vector<Square> diagonal;
    code = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        if (off_A1H8(s) < 0 && file_of(s) <= FILE_D && rank_of(s) <= RANK_4)
            MapA1D1D4[s] = code++;

        else if (!off_A1H8(s) && file_of(s) <= FILE_D)
            diagonal.push_back(s);

    // Diagonal squares are encoded as last ones
    for (auto s : diagonal)
        MapA1D1D4[s] = code++;

    // Compute KK_idx[] that encodes all the 461 possible legal positions of a couple of
    // kings where first king is on a1-d1-d4 triangle. When first king is on the a1-d4
    // diagonal, second king is assumed not to be above the a1-h8 diagonal.
    std::vector<std::pair<int, Square>> bothOnDiagonal;
    code = 0;
    for (int idx = 0; idx < 10; idx++)
        for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
            if (idx == MapA1D1D4[s1] && (idx || s1 == SQ_B1)) // SQ_B1 is mapped to 0
                for (Square s2 = SQ_A1; s2 <= SQ_H8; ++s2)
                {
                    if ((StepAttacksBB[KING][s1] | s1) & s2) // Illegal position
                        KK_idx[idx][s2] = -1;

                    else if (!off_A1H8(s1) && off_A1H8(s2) > 0)
                        KK_idx[idx][s2] = -1; // First king on diagonal, second above

                    else if (!off_A1H8(s1) && !off_A1H8(s2))
                        bothOnDiagonal.push_back(std::make_pair(idx, s2));

                    else
                        KK_idx[idx][s2] = code++;
                }

    // Legal positions with both kings on diagonal are encoded as last ones
    for (auto p : bothOnDiagonal)
        KK_idx[p.first][p.second] = code++;

    for (PieceType p1 = PAWN; p1 < KING; ++p1) {
        WDLHash.insert({KING, p1, KING});

        for (PieceType p2 = PAWN; p2 <= p1; ++p2) {
            WDLHash.insert({KING, p1, p2, KING});
            WDLHash.insert({KING, p1, KING, p2});

            for (PieceType p3 = PAWN; p3 < KING; ++p3)
                WDLHash.insert({KING, p1, p2, KING, p3});

            for (PieceType p3 = PAWN; p3 <= p2; ++p3) {
                WDLHash.insert({KING, p1, p2, p3, KING});

                for (PieceType p4 = PAWN; p4 <= p3; ++p4)
                    WDLHash.insert({KING, p1, p2, p3, p4, KING});

                for (PieceType p4 = PAWN; p4 < KING; ++p4)
                    WDLHash.insert({KING, p1, p2, p3, KING, p4});
            }

            for (PieceType p3 = PAWN; p3 <= p1; ++p3)
                for (PieceType p4 = PAWN; p4 <= (p1 == p3 ? p2 : p3); ++p4)
                    WDLHash.insert({KING, p1, p2, KING, p3, p4});
        }
    }

    std::cerr << "info string Found " << WDLTable.size() << " tablebases" << std::endl;
}

// Probe the WDL table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
WDLScore Tablebases::probe_wdl(Position& pos, int *success)
{
    *success = 1;
    WDLScore v = probe_ab(pos, WDLLoss, WDLWin, success);

    // If en passant is not possible, we are done.
    if (pos.ep_square() == SQ_NONE)
        return v;

    if (*success == 0)
        return WDLDraw;

    // Now handle en passant.
    WDLScore v1 = WDLScore(-3); // FIXME use a proper enum value here
    // Generate (at least) all legal en passant captures.
    ExtMove stack[MAX_MOVES];
    ExtMove *moves, *end;
    StateInfo st;

    if (!pos.checkers())
        end = generate<CAPTURES>(pos, stack);
    else
        end = generate<EVASIONS>(pos, stack);

    CheckInfo ci(pos);

    for (moves = stack; moves < end; ++moves) {
        Move capture = moves->move;

        if (type_of(capture) != ENPASSANT
                || !pos.legal(capture, ci.pinned))
            continue;

        pos.do_move(capture, st, pos.gives_check(capture, ci));
        WDLScore v0 = -probe_ab(pos, WDLLoss, WDLWin, success);
        pos.undo_move(capture);

        if (*success == 0)
            return WDLDraw;

        if (v0 > v1) v1 = v0;
    }

    if (v1 > -3) {
        if (v1 >= v) v = v1;
        else if (v == 0) {
            // Check whether there is at least one legal non-ep move.
            for (moves = stack; moves < end; ++moves) {
                Move capture = moves->move;

                if (type_of(capture) == ENPASSANT) continue;

                if (pos.legal(capture, ci.pinned))
                    break;
            }

            if (moves == end && !pos.checkers()) {
                end = generate<QUIETS>(pos, end);

                for (; moves < end; ++moves) {
                    Move move = moves->move;

                    if (pos.legal(move, ci.pinned))
                        break;
                }
            }

            // If not, then we are forced to play the losing ep capture.
            if (moves == end)
                v = v1;
        }
    }

    return v;
}

// Check whether there has been at least one repetition of positions
// since the last capture or pawn move.
static int has_repeated(StateInfo *st)
{
    while (1) {
        int i = 4, e = std::min(st->rule50, st->pliesFromNull);

        if (e < i)
            return 0;

        StateInfo *stp = st->previous->previous;

        do {
            stp = stp->previous->previous;

            if (stp->key == st->key)
                return 1;

            i += 2;
        } while (i <= e);

        st = st->previous;
    }
}

// Use the DTZ tables to filter out moves that don't preserve the win or draw.
// If the position is lost, but DTZ is fairly high, only keep moves that
// maximise DTZ.
//
// A return value false indicates that not all probes were successful and that
// no moves were filtered out.
bool Tablebases::root_probe(Position& pos, Search::RootMoves& rootMoves, Value& score)
{
    int success;
    int dtz = probe_dtz(pos, &success);

    if (!success)
        return false;

    StateInfo st;
    CheckInfo ci(pos);

    // Probe each move
    for (size_t i = 0; i < rootMoves.size(); ++i) {
        Move move = rootMoves[i].pv[0];
        pos.do_move(move, st, pos.gives_check(move, ci));
        int v = 0;

        if (pos.checkers() && dtz > 0) {
            ExtMove s[MAX_MOVES];

            if (generate<LEGAL>(pos, s) == s)
                v = 1;
        }

        if (!v) {
            if (st.rule50 != 0) {
                v = -probe_dtz(pos, &success);

                if (v > 0)
                    ++v;
                else if (v < 0)
                    --v;
            } else {
                v = -probe_wdl(pos, &success);
                v = wdl_to_dtz[v + 2];
            }
        }

        pos.undo_move(move);

        if (!success)
            return false;

        rootMoves[i].score = (Value)v;
    }

    // Obtain 50-move counter for the root position.
    // In Stockfish there seems to be no clean way, so we do it like this:
    int cnt50 = st.previous->rule50;

    // Use 50-move counter to determine whether the root position is
    // won, lost or drawn.
    int wdl = 0;

    if (dtz > 0)
        wdl = (dtz + cnt50 <= 100) ? 2 : 1;
    else if (dtz < 0)
        wdl = (-dtz + cnt50 <= 100) ? -2 : -1;

    // Determine the score to report to the user.
    score = WDL_to_value[wdl + 2];

    // If the position is winning or losing, but too few moves left, adjust the
    // score to show how close it is to winning or losing.
    // NOTE: int(PawnValueEg) is used as scaling factor in score_to_uci().
    if (wdl == 1 && dtz <= 100)
        score = (Value)(((200 - dtz - cnt50) * int(PawnValueEg)) / 200);
    else if (wdl == -1 && dtz >= -100)
        score = -(Value)(((200 + dtz - cnt50) * int(PawnValueEg)) / 200);

    // Now be a bit smart about filtering out moves.
    size_t j = 0;

    if (dtz > 0) { // winning (or 50-move rule draw)
        int best = 0xffff;

        for (size_t i = 0; i < rootMoves.size(); ++i) {
            int v = rootMoves[i].score;

            if (v > 0 && v < best)
                best = v;
        }

        int max = best;

        // If the current phase has not seen repetitions, then try all moves
        // that stay safely within the 50-move budget, if there are any.
        if (!has_repeated(st.previous) && best + cnt50 <= 99)
            max = 99 - cnt50;

        for (size_t i = 0; i < rootMoves.size(); ++i) {
            int v = rootMoves[i].score;

            if (v > 0 && v <= max)
                rootMoves[j++] = rootMoves[i];
        }
    } else if (dtz < 0) { // losing (or 50-move rule draw)
        int best = 0;

        for (size_t i = 0; i < rootMoves.size(); ++i) {
            int v = rootMoves[i].score;

            if (v < best)
                best = v;
        }

        // Try all moves, unless we approach or have a 50-move rule draw.
        if (-best * 2 + cnt50 < 100)
            return true;

        for (size_t i = 0; i < rootMoves.size(); ++i) {
            if (rootMoves[i].score == best)
                rootMoves[j++] = rootMoves[i];
        }
    } else { // drawing
        // Try all moves that preserve the draw.
        for (size_t i = 0; i < rootMoves.size(); ++i) {
            if (rootMoves[i].score == 0)
                rootMoves[j++] = rootMoves[i];
        }
    }

    rootMoves.resize(j, Search::RootMove(MOVE_NONE));

    return true;
}

// Use the WDL tables to filter out moves that don't preserve the win or draw.
// This is a fallback for the case that some or all DTZ tables are missing.
//
// A return value false indicates that not all probes were successful and that
// no moves were filtered out.
bool Tablebases::root_probe_wdl(Position& pos, Search::RootMoves& rootMoves, Value& score)
{
    int success;

    WDLScore wdl = Tablebases::probe_wdl(pos, &success);

    if (!success)
        return false;

    score = WDL_to_value[wdl + 2];

    StateInfo st;
    CheckInfo ci(pos);

    int best = WDLLoss;

    // Probe each move
    for (size_t i = 0; i < rootMoves.size(); ++i) {
        Move move = rootMoves[i].pv[0];
        pos.do_move(move, st, pos.gives_check(move, ci));
        WDLScore v = -Tablebases::probe_wdl(pos, &success);
        pos.undo_move(move);

        if (!success)
            return false;

        rootMoves[i].score = (Value)v;

        if (v > best)
            best = v;
    }

    size_t j = 0;

    for (size_t i = 0; i < rootMoves.size(); ++i) {
        if (rootMoves[i].score == best)
            rootMoves[j++] = rootMoves[i];
    }

    rootMoves.resize(j, Search::RootMove(MOVE_NONE));

    return true;
}
