#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

using std::array;
using std::cerr;
using std::cout;
using std::ifstream;
using std::ofstream;
using std::string;

class BitWriter {
public:
    explicit BitWriter(ofstream& out) : out_(out) {}

    void writeBit(bool b) {
        buf_ = (buf_ << 1) | (b ? 1 : 0);
        bits_++;
        totalBits_++;
        if (bits_ == 8) flushByte();
    }

    void flushFinal() {
        if (bits_ == 0) return;
        buf_ <<= (8 - bits_);
        flushByte();
    }

    uint64_t totalBits() const { return totalBits_; }

private:
    void flushByte() {
        out_.put(static_cast<char>(buf_));
        buf_ = 0;
        bits_ = 0;
    }

    ofstream& out_;
    uint8_t buf_{0};
    int bits_{0};
    uint64_t totalBits_{0};
};

class BitReader {
public:
    explicit BitReader(ifstream& in) : in_(in) {}

    bool readBit(bool& bit) {
        if (bitsLeft_ == 0) {
            char c;
            if (!in_.get(c)) return false;
            buf_ = static_cast<uint8_t>(c);
            bitsLeft_ = 8;
        }
        bit = ((buf_ >> (bitsLeft_ - 1)) & 1) != 0;
        bitsLeft_--;
        return true;
    }

private:
    ifstream& in_;
    uint8_t buf_{0};
    int bitsLeft_{0};
};

static bool readWholeFile(const string& path, std::vector<uint8_t>& data) {
    ifstream in(path, std::ios::binary);
    if (!in) return false;

    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);

    data.resize(static_cast<size_t>(sz));
    if (sz > 0) in.read(reinterpret_cast<char*>(data.data()), sz);

    return true;
}

static uint64_t fileSize(const string& path) {
    ifstream in(path, std::ios::binary | std::ios::ate);
    return in ? static_cast<uint64_t>(in.tellg()) : 0ULL;
}

static void buildCum(const array<uint32_t, 256>& freq,
                     array<uint32_t, 257>& cum,
                     uint32_t& total) {
    cum[0] = 0;
    uint64_t sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += freq[i];
        cum[i + 1] = static_cast<uint32_t>(sum);
    }
    total = cum[256];
}

static int findSymbol(uint32_t scaled, const array<uint32_t, 257>& cum) {
    for (int s = 0; s < 256; s++) {
        if (scaled < cum[s + 1]) return s;
    }
    return 255;
}

static void compressArithmetic(const string& inPath, const string& outPath) {
    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();

    std::vector<uint8_t> data;
    if (!readWholeFile(inPath, data)) {
        cerr << "Cannot open input: " << inPath << "\n";
        return;
    }
    if (data.empty()) {
        cerr << "Input is empty.\n";
        return;
    }

    array<uint32_t, 256> freq{};
    for (uint8_t b : data) freq[b]++;

    array<uint32_t, 257> cum{};
    uint32_t total = 0;
    buildCum(freq, cum, total);
    if (total == 0) {
        cerr << "Bad total.\n";
        return;
    }

    constexpr uint32_t BITS = 32;
    constexpr uint64_t MAX_VALUE = (1ULL << BITS) - 1;
    constexpr uint64_t HALF = (MAX_VALUE / 2) + 1;
    constexpr uint64_t QUARTER = HALF / 2;
    constexpr uint64_t THREE_QUARTERS = QUARTER * 3;

    uint64_t low = 0;
    uint64_t high = MAX_VALUE;
    uint32_t pending = 0;

    ofstream out(outPath, std::ios::binary);
    if (!out) {
        cerr << "Cannot create output: " << outPath << "\n";
        return;
    }

    const uint32_t magic = 0x41524331;
    const uint32_t origSize = static_cast<uint32_t>(data.size());

    uint64_t encodedBitCount = 0;

    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&origSize), sizeof(origSize));
    out.write(reinterpret_cast<const char*>(freq.data()), sizeof(uint32_t) * 256);

    std::streampos bitCountPos = out.tellp();
    out.write(reinterpret_cast<const char*>(&encodedBitCount), sizeof(encodedBitCount));

    BitWriter bw(out);

    auto outputBit = [&](bool bit) {
        bw.writeBit(bit);
        while (pending > 0) {
            bw.writeBit(!bit);
            pending--;
        }
    };

    for (uint8_t sym : data) {
        uint64_t range = high - low + 1;
        uint32_t s = sym;

        uint64_t newHigh = low + (range * cum[s + 1]) / total - 1;
        uint64_t newLow  = low + (range * cum[s])     / total;
        low = newLow;
        high = newHigh;

        while (true) {
            if (high < HALF) {
                outputBit(false);
            } else if (low >= HALF) {
                outputBit(true);
                low -= HALF;
                high -= HALF;
            } else if (low >= QUARTER && high < THREE_QUARTERS) {
                pending++;
                low -= QUARTER;
                high -= QUARTER;
            } else {
                break;
            }

            low <<= 1;
            high = (high << 1) | 1;
        }
    }

    pending++;
    if (low < QUARTER) outputBit(false);
    else outputBit(true);

    bw.flushFinal();

    encodedBitCount = bw.totalBits();
    out.seekp(bitCountPos);
    out.write(reinterpret_cast<const char*>(&encodedBitCount), sizeof(encodedBitCount));
    out.close();

    auto t1 = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    uint64_t inSz = data.size();
    uint64_t outSz = fileSize(outPath);
    double ratio = (1.0 - (double)outSz / (double)inSz) * 100.0;

    cout << "Compressed OK\n";
    cout << "Input:  " << inSz << " bytes\n";
    cout << "Output: " << outSz << " bytes\n";
    cout << "Compression: " << ratio << "%\n";
    cout << "Time: " << ms << " ms\n";
}

static void decompressArithmetic(const string& inPath, const string& outPath) {
    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();

    ifstream in(inPath, std::ios::binary);
    if (!in) {
        cerr << "Cannot open input: " << inPath << "\n";
        return;
    }

    uint32_t magic = 0;
    uint32_t origSize = 0;

    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&origSize), sizeof(origSize));
    if (!in || magic != 0x41524331) {
        cerr << "Bad format.\n";
        return;
    }

    array<uint32_t, 256> freq{};
    in.read(reinterpret_cast<char*>(freq.data()), sizeof(uint32_t) * 256);

    uint64_t encodedBitCount = 0;
    in.read(reinterpret_cast<char*>(&encodedBitCount), sizeof(encodedBitCount));
    if (!in) {
        cerr << "Header read error.\n";
        return;
    }

    array<uint32_t, 257> cum{};
    uint32_t total = 0;
    buildCum(freq, cum, total);
    if (total == 0) {
        cerr << "Bad total.\n";
        return;
    }

    constexpr uint32_t BITS = 32;
    constexpr uint64_t MAX_VALUE = (1ULL << BITS) - 1;
    constexpr uint64_t HALF = (MAX_VALUE / 2) + 1;
    constexpr uint64_t QUARTER = HALF / 2;
    constexpr uint64_t THREE_QUARTERS = QUARTER * 3;

    uint64_t low = 0;
    uint64_t high = MAX_VALUE;
    uint64_t value = 0;

    BitReader br(in);
    uint64_t bitsRead = 0;

    auto readOneBit = [&]() -> uint64_t {
        bool bit = false;
        if (bitsRead < encodedBitCount && br.readBit(bit)) {
            bitsRead++;
            return bit ? 1ULL : 0ULL;
        }
        bitsRead++;
        return 0ULL;
    };

    for (uint32_t i = 0; i < BITS; i++) value = (value << 1) | readOneBit();

    ofstream out(outPath, std::ios::binary);
    if (!out) {
        cerr << "Cannot create output: " << outPath << "\n";
        return;
    }

    for (uint32_t produced = 0; produced < origSize; produced++) {
        uint64_t range = high - low + 1;

        uint64_t scaled = ((value - low + 1) * total - 1) / range;
        int sym = findSymbol(static_cast<uint32_t>(scaled), cum);

        out.put(static_cast<char>(static_cast<uint8_t>(sym)));

        uint64_t newHigh = low + (range * cum[sym + 1]) / total - 1;
        uint64_t newLow  = low + (range * cum[sym])     / total;
        low = newLow;
        high = newHigh;

        while (true) {
            if (high < HALF) {
            } else if (low >= HALF) {
                low -= HALF;
                high -= HALF;
                value -= HALF;
            } else if (low >= QUARTER && high < THREE_QUARTERS) {
                low -= QUARTER;
                high -= QUARTER;
                value -= QUARTER;
            } else {
                break;
            }

            low <<= 1;
            high = (high << 1) | 1;
            value = (value << 1) | readOneBit();
        }
    }

    out.close();

    auto t1 = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    cout << "Decompressed OK\n";
    cout << "Time: " << ms << " ms\n";
}

int main() {
    cout << "1) Compress (Arithmetic)\n2) Decompress (Arithmetic)\nChoose: ";
    int choice = 0;
    std::cin >> choice;

    string inFile, outFile;
    cout << "Input file: ";
    std::cin >> inFile;
    cout << "Output file: ";
    std::cin >> outFile;

    if (choice == 1) compressArithmetic(inFile, outFile);
    else if (choice == 2) decompressArithmetic(inFile, outFile);
    else cout << "Wrong choice\n";

    return 0;
}

/*
buildCum:
- по массиву частот строит cum (префиксные суммы) и total (общее число байтов).
- cum используется для перевода символа в интервал на оси вероятностей.

compressArithmetic:
- использует целочисленный диапазон 32 бита [0 .. 2^32-1].
- для каждого символа сужает [low, high] по cum.
- нормализация:
  1) high < HALF  -> выводим 0
  2) low  >= HALF -> выводим 1 и сдвигаем интервал вниз на HALF
  3) low >= QUARTER и high < THREE_QUARTERS -> underflow, увеличиваем pending
- pending хранит число “отложенных” битов, которые будут выведены инверсией после определения следующего стабильного бита.
- в конце дописывает завершающие биты и сохраняет количество полезных бит encodedBitCount.

decompressArithmetic:
- читает заголовок и восстанавливает cum.
- value инициализируется первыми 32 битами потока.
- scaled вычисляет позицию value внутри текущего интервала и переводит её в диапазон [0..total).
- по scaled выбирается символ (по cum), затем обновляются low/high и делается нормализация, подтягивая новые биты.
- декодирование останавливается ровно после origSize байт.
*/
