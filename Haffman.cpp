#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

using std::array;
using std::cerr;
using std::cout;
using std::ifstream;
using std::ofstream;
using std::string;

/*
Лабораторная работа 1: Код Хаффмана

Формат выходного файла:
1) magic (uint32)          — идентификатор формата
2) origSize (uint64)       — размер исходного файла (в байтах)
3) uniqueCount (uint16)    — число уникальных символов
4) Таблица частот:
   для каждого уникального символа: [symbol:uint8][freq:uint64]
5) Закодированные данные (битовый поток), упакованные в байты

Кодирование:
- читаем входной файл как байты,
- считаем частоты,
- строим дерево Хаффмана через очередь с приоритетом (берём два минимальных узла),
- строим коды обходом дерева,
- записываем заголовок, таблицу частот и битовый поток.

Декодирование:
- читаем заголовок и таблицу частот,
- восстанавливаем дерево,
- читаем биты и идём по дереву до листа, получая символ,
- останавливаемся после origSize байт.
*/

/* Узел дерева Хаффмана */
struct Node {
    uint8_t ch{};
    uint64_t freq{};
    Node* left{nullptr};
    Node* right{nullptr};
    bool is_leaf{false};
};

/* Компаратор для priority_queue: на вершине узел с минимальной частотой */
struct NodeCmp {
    bool operator()(const Node* a, const Node* b) const {
        return a->freq > b->freq;
    }
};

/* Освобождение памяти дерева */
static void freeTree(Node* n) {
    if (!n) return;
    freeTree(n->left);
    freeTree(n->right);
    delete n;
}

/* Запись битов в файл: накапливаем 8 бит -> записываем 1 байт */
class BitWriter {
public:
    explicit BitWriter(ofstream& out) : out_(out) {}

    void writeBit(bool b) {
        buffer_ = (buffer_ << 1) | (b ? 1 : 0);
        bits_++;
        if (bits_ == 8) flushByte();
    }

    void writeBitsFromString(const string& bits01) {
        for (char c : bits01) writeBit(c == '1');
    }

    /* Дописываем нули до целого байта и сбрасываем остаток */
    void flushFinal() {
        if (bits_ == 0) return;
        buffer_ <<= (8 - bits_);
        flushByte();
    }

private:
    void flushByte() {
        out_.put(static_cast<char>(buffer_));
        buffer_ = 0;
        bits_ = 0;
    }

    ofstream& out_;
    uint8_t buffer_{0};
    int bits_{0};
};

/* Чтение битов: читаем байт и выдаём биты по одному */
class BitReader {
public:
    explicit BitReader(ifstream& in) : in_(in) {}

    bool readBit(bool& bit) {
        if (bits_left_ == 0) {
            char c;
            if (!in_.get(c)) return false;
            buffer_ = static_cast<uint8_t>(c);
            bits_left_ = 8;
        }
        bit = ((buffer_ >> (bits_left_ - 1)) & 1) != 0;
        bits_left_--;
        return true;
    }

private:
    ifstream& in_;
    uint8_t buffer_{0};
    int bits_left_{0};
};

/* Обход дерева и построение кодов: влево -> '0', вправо -> '1' */
static void buildCodes(Node* n, string cur, array<string, 256>& codes) {
    if (!n) return;

    if (n->is_leaf) {
        /* Случай файла из одного символа: пустой путь заменяем на "0" */
        codes[n->ch] = cur.empty() ? "0" : cur;
        return;
    }

    buildCodes(n->left, cur + "0", codes);
    buildCodes(n->right, cur + "1", codes);
}

/* Построение дерева Хаффмана по частотам */
static Node* buildHuffmanTree(const array<uint64_t, 256>& freq, uint16_t& uniqueCount) {
    std::priority_queue<Node*, std::vector<Node*>, NodeCmp> pq;
    uniqueCount = 0;

    /* 1) Создаём листья для всех символов с ненулевой частотой */
    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            uniqueCount++;
            Node* leaf = new Node{};
            leaf->ch = static_cast<uint8_t>(i);
            leaf->freq = freq[i];
            leaf->is_leaf = true;
            pq.push(leaf);
        }
    }

    if (pq.empty()) return nullptr;

    /* 2) Если символ один — дерево состоит из одного листа */
    if (pq.size() == 1) return pq.top();

    /* 3) Объединяем два минимальных узла, пока не останется один корень */
    while (pq.size() > 1) {
        Node* a = pq.top(); pq.pop();
        Node* b = pq.top(); pq.pop();

        Node* parent = new Node{};
        parent->freq = a->freq + b->freq;
        parent->left = a;
        parent->right = b;
        parent->is_leaf = false;

        pq.push(parent);
    }

    return pq.top();
}

/* Чтение входного файла целиком в память */
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

/* Получение размера файла для статистики */
static uint64_t fileSize(const string& path) {
    ifstream in(path, std::ios::binary | std::ios::ate);
    return in ? static_cast<uint64_t>(in.tellg()) : 0ULL;
}

/* Кодирование файла */
static void encodeFile(const string& inPath, const string& outPath) {
    /* 1) Читаем входной файл */
    std::vector<uint8_t> data;

    if (!readWholeFile(inPath, data)) {
        cerr << "Cannot open input: " << inPath << "\n";
        return;
    }
    if (data.empty()) {
        cerr << "Input is empty.\n";
        return;
    }

    /* 2) Считаем частоты */
    array<uint64_t, 256> freq{};
    for (uint8_t b : data) freq[b]++;

    /* 3) Строим дерево и таблицу кодов */
    uint16_t uniqueCount = 0;
    Node* root = buildHuffmanTree(freq, uniqueCount);
    if (!root) {
        cerr << "Tree build error.\n";
        return;
    }

    array<string, 256> codes{};
    buildCodes(root, "", codes);

    /* 4) Открываем выходной файл и пишем заголовок + таблицу частот */
    ofstream out(outPath, std::ios::binary);
    if (!out) {
        cerr << "Cannot create output: " << outPath << "\n";
        freeTree(root);
        return;
    }

    const uint32_t magic = 0x48464631;                 // "HFF1"
    const uint64_t origSize = static_cast<uint64_t>(data.size());

    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&origSize), sizeof(origSize));
    out.write(reinterpret_cast<const char*>(&uniqueCount), sizeof(uniqueCount));

    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            uint8_t sym = static_cast<uint8_t>(i);
            out.put(static_cast<char>(sym));
            out.write(reinterpret_cast<const char*>(&freq[i]), sizeof(uint64_t));
        }
    }

    /* 5) Пишем закодированные данные как битовый поток */
    BitWriter bw(out);
    for (uint8_t b : data) bw.writeBitsFromString(codes[b]);
    bw.flushFinal();

    out.close();

    /* 6) Вывод статистики */
    uint64_t inSz = static_cast<uint64_t>(data.size());
    uint64_t outSz = fileSize(outPath);
    double ratio = (1.0 - (double)outSz / (double)inSz) * 100.0;

    cout << "Encoded OK\n";
    cout << "Input:  " << inSz << " bytes\n";
    cout << "Output: " << outSz << " bytes\n";
    cout << "Compression: " << ratio << "%\n";

    freeTree(root);
}

/* Декодирование файла */
static void decodeFile(const string& inPath, const string& outPath) {
    /* 1) Открываем сжатый файл */
    ifstream in(inPath, std::ios::binary);
    if (!in) {
        cerr << "Cannot open encoded file: " << inPath << "\n";
        return;
    }

    /* 2) Читаем заголовок */
    uint32_t magic = 0;
    uint64_t origSize = 0;
    uint16_t uniqueCount = 0;

    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&origSize), sizeof(origSize));
    in.read(reinterpret_cast<char*>(&uniqueCount), sizeof(uniqueCount));

    if (!in || magic != 0x48464631) {
        cerr << "Bad format.\n";
        return;
    }

    /* 3) Читаем таблицу частот */
    array<uint64_t, 256> freq{};
    for (uint16_t i = 0; i < uniqueCount; i++) {
        char symC;
        uint64_t f;
        in.get(symC);
        in.read(reinterpret_cast<char*>(&f), sizeof(uint64_t));
        freq[static_cast<uint8_t>(symC)] = f;
    }

    /* 4) Восстанавливаем дерево по частотам */
    uint16_t dummyUnique = 0;
    Node* root = buildHuffmanTree(freq, dummyUnique);
    if (!root) {
        cerr << "Tree build error.\n";
        return;
    }

    /* 5) Открываем выходной файл */
    ofstream out(outPath, std::ios::binary);
    if (!out) {
        cerr << "Cannot create output: " << outPath << "\n";
        freeTree(root);
        return;
    }

    /* 6) Частный случай: один символ во всём файле */
    if (root->is_leaf) {
        for (uint64_t i = 0; i < origSize; i++) out.put(static_cast<char>(root->ch));
        out.close();
        freeTree(root);
        cout << "Decoded OK\n";
        return;
    }

    /* 7) Читаем биты и идём по дереву до восстановления origSize байт */
    BitReader br(in);
    Node* cur = root;
    uint64_t written = 0;

    while (written < origSize) {
        bool bit;
        if (!br.readBit(bit)) {
            cerr << "Unexpected EOF in bitstream.\n";
            break;
        }

        cur = bit ? cur->right : cur->left;

        if (cur->is_leaf) {
            out.put(static_cast<char>(cur->ch));
            written++;
            cur = root;
        }
    }

    out.close();
    freeTree(root);

    if (written == origSize) cout << "Decoded OK\n";
    else cout << "Decoded with mismatch: " << written << "/" << origSize << "\n";
}

/* Меню программы: выбор режима и ввод имён файлов */
int main() {
    cout << "1) Encode (Huffman)\n2) Decode (Huffman)\nChoose: ";
    int choice = 0;
    std::cin >> choice;

    string inFile, outFile;
    cout << "Input file: ";
    std::cin >> inFile;
    cout << "Output file: ";
    std::cin >> outFile;

    if (choice == 1) encodeFile(inFile, outFile);
    else if (choice == 2) decodeFile(inFile, outFile);
    else cout << "Wrong choice\n";

    return 0;
}
