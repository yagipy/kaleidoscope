#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// レキサー

enum Token {
    token_eof = -1,

    // commands
    token_def = -2,
    token_extern = -3,

    // primary
    token_identifier = -4,
    token_number = -5
};

static std::string IdentifierStr; // Filled in if token_identifier
static double NumVal; // Filled in if token_number

static int getToken() {
    static int LastChar = ' ';

    // skip any whitespace
    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
        IdentifierStr = LastChar;
        while (isalnum((LastChar = getchar())))
            IdentifierStr += LastChar;

        if (IdentifierStr == "def")
            return token_def;
        if (IdentifierStr == "extern")
            return token_extern;
        return token_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), nullptr);
        return token_number;
    }

    if (LastChar == '#') {
        // Comment until end of line.
        do {
            LastChar = getchar();
        } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar != EOF)
            return getToken();
    }

    // Chack for end of file, Don't eat the EOF.
    if (LastChar == EOF)
        return token_eof;

    // Otherwise, just return the character as its ascii value.
    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}

// ast
// 式、プロトタイプ、関数オブジェクトが存在する
namespace {
    // 式
    class ExprAST {
    public:
        virtual ~ExprAST() = default;
    };

    // 数値リテラル
    class NumberExprAST: public ExprAST {
        double Val; // 数値
    public:
        NumberExprAST(double Val): Val(Val) {}
    };

    // 変数
    class VariableExprAST: public ExprAST {
        std::string Name; // 変数名
    public:
        VariableExprAST(const std::string &Name): Name(Name) {}
    };

    // 二項演算子(binary operator)
    class BinaryExprAST: public ExprAST {
        char Op; // オペコード(例: '+')
        std::unique_ptr<ExprAST> LHS, RHS;
    public:
        BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS): Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
    };

    // 関数呼び出し
    class CallExprAST: public ExprAST {
        std::string Callee; // 関数名
        std::vector<std::unique_ptr<ExprAST>> Args; // 引数式
    public:
        CallExprAST(const std::string &Callee, std::vector<std::unique_ptr<ExprAST>> Args): Callee(Callee), Args(std::move(Args)) {}
    };

    // 関数のプロトタイプ(インターフェース)
    class PrototypeAST {
        std::string Name; // 関数名
        std::vector<std::string> Args; // 引数名
    public:
        PrototypeAST(const std::string &Name, std::vector<std::string> Args): Name(Name), Args(std::move(Args)) {}
        const std::string &getName() const { return Name; }
    };

    // 関数
    class FunctionAST {
        std::unique_ptr<PrototypeAST> Proto;
        std::unique_ptr<ExprAST> Body;
    public:
        FunctionAST(std::unique_ptr<PrototypeAST> Proto, std::unique_ptr<ExprAST> Body): Proto(std::move(Proto)), Body(std::move(Body)) {}
    };
}

// parser

// パーサーが現在見ているトークン
static int CurrentToken;
// レキサーから別のトークンを読み込み、CurrentTokenを更新する
static int getNextToken() {
    return CurrentToken = getToken();
}

// 二項演算子の優先順位
static std::map<char, int> BinaryOperatorPrecedence;

// 現在のトークンの優先順位が返却される
// 二項演算子でない場合は-1が返却される
static int GetTokenPrecedence() {
    if (!isascii(CurrentToken))
        return -1;

    int TokenPrecedence = BinaryOperatorPrecedence[CurrentToken];
    if (TokenPrecedence <= 0)
        return -1;
    return TokenPrecedence;
}

std::unique_ptr<ExprAST> LogError(const char *Str) {
    fprintf(stderr, "Error: %s\n", Str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
    LogError(Str);
    return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

// 数値リテラルをパース
// CurrentTokenがtoken_numberの際に呼び出される
// 現在の数値を取り、NumberExprASTノードを作成し、レキサーを次のトークンに進め、最後に返却する
static std::unique_ptr<ExprAST> ParseNumberExpr() {
    auto Result = std::make_unique<NumberExprAST>(NumVal);
    getNextToken();
    return std::move(Result);
}

// 括弧演算子をパース
// CurrentTokenが'('の際に呼び出される
// 再帰になっている
static std::unique_ptr<ExprAST> ParseParenExpr() {
    getNextToken();
    auto V = ParseExpression();
    if (!V)
        return nullptr;

    if (CurrentToken != ')') // ')'がない場合(括弧が閉じられていない場合)
        return LogError("expected ')'");
    getNextToken();
    return V;
}

// 識別子をパース
// CurrentTokenがtoken_identifierの際に呼び出される([a-zA-Z][a-zA-Z0-9]*)
// 変数参照か関数呼び出し式かを判断している
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;

    getNextToken(); // 識別子を読み進める

    if (CurrentToken != '(') // 変数参照の場合
        return std::make_unique<VariableExprAST>(IdName);

    // 関数呼び出し式の場合
    getNextToken(); // '('を読み進める
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurrentToken != ')') {
        while (true) {
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;
            if (CurrentToken == ')')
                break;
            if (CurrentToken != ',')
                return LogError("Expected ')' or ',' in argument list");
            getNextToken();
        }
    }
    getNextToken();
    return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

// 任意の一次式をパース
static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurrentToken) {
        default:
            return LogError("unknown token when expecting an expression");
        case token_identifier:
            return ParseIdentifierExpr();
        case token_number:
            return ParseNumberExpr();
        case '(':
            return ParseParenExpr();
    }
}

// ペアのシーケンスをパースする
// 優先順位とこれまでにパースされた部分の式へのポインタを受け取る
// 渡される優先順位の値は、この関数が処理することができる最小の演算子の優先順位
// BinaryOperatorRHSは空も許容し、その場合はLHSを返す
static std::unique_ptr<ExprAST> ParseBinaryOperatorRHS(int ExprPrecedence, std::unique_ptr<ExprAST> LHS) {
    while (true) {
        int TokenPrecedence = GetTokenPrecedence();
        if (TokenPrecedence < ExprPrecedence) // 現在のトークンの優先順位が引数で渡された優先順位より低い場合
            return LHS;
        // 上記により、これ以降はトークンが二項演算子であることがわかる

        int BinaryOperator = CurrentToken;
        getNextToken(); // 二項演算子を読み進める

        auto RHS = ParsePrimary();
        if (!RHS)
            return nullptr;

        int NextPrecedence = GetTokenPrecedence();
        if (TokenPrecedence < NextPrecedence) { // 前の二項演算子がCurrentが指している二項演算子より小さい場合(例: a + b * c)
            // a + (b binop unparsed)
            RHS = ParseBinaryOperatorRHS(TokenPrecedence + 1, std::move(RHS)); // '+'よりも高い優先順位のペアのシーケンスは、一緒に解析されてRHSとして返される
            if (!RHS)
                return nullptr;
        }

        // 同じか大きい場合(例: a + b + c, a * b + c)
        // (a+b) binop unparsed
        LHS = std::make_unique<BinaryExprAST>(BinaryOperator, std::move(LHS), std::move(RHS));
    }
}

// 式は、一次式の後に[binary operator, primary expr]のペアのシーケンスが続くものとする
// 二項演算子で区切られた一次式の流れとして考える
// "a+b+(c+d)*e*f+g" => 「a」を解析し、次に[+, b] [+, (c+d)] [*, e] [*, f] [+, g] のペアを順番に見ていく
// 括弧は一次式なので、二項演算子のパーサーは考慮する必要がない
static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary(); // 上記例の'a'をパース(CurrentTokenは'+'になる)
    if (!LHS)
        return nullptr;

    return ParseBinaryOperatorRHS(0, std::move(LHS));
}

// 関数のプロトタイプをパース
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    if (CurrentToken != token_identifier)
        return LogErrorP("Expected function name in prototype");

    std::string FunctionName = IdentifierStr;
    getNextToken();

    if (CurrentToken != '(')
        return LogErrorP("Expected '(' in prototype");

    // 引数名のリストを読み込む
    std::vector<std::string> ArgNames;
    while (getNextToken() == token_identifier)
        ArgNames.push_back(IdentifierStr);
    if (CurrentToken != ')')
        return LogErrorP("Expected ')' in prototype");

    getNextToken();

    return std::make_unique<PrototypeAST>(FunctionName, std::move(ArgNames));
}

// 関数定義をパース
static std::unique_ptr<FunctionAST> ParseDefinition() {
    getNextToken();
    auto Proto = ParsePrototype();
    if (!Proto)
        return nullptr;

    if (auto E = ParseExpression())
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    return nullptr;
}

// トップレベルの式をそのまま評価できるようにする
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseExpression()) {
        auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

static std::unique_ptr<PrototypeAST> ParseExtern() {
    getNextToken();
    return ParsePrototype();
}

// top level parsing

static void HandleDefinition() {
    if (ParseDefinition()) {
        fprintf(stderr, "Parsed a function definition\n");
    } else {
        getNextToken();
    }
}

static void HandleExtern() {
    if (ParseExtern()) {
        fprintf(stderr, "Parsed an extern\n");
    } else {
        getNextToken();
    }
}

static void HandleTopLevelExpression() {
    if (ParseTopLevelExpr()) {
        fprintf(stderr, "Parsed a top-level expr\n");
    } else {
        getNextToken();
    }
}

static void MainLoop() {
    while (true) {
        fprintf(stderr, "ready> ");
        switch (CurrentToken) {
            case token_eof:
                return;
            case ';':
                getNextToken();
                break;
            case token_def:
                HandleDefinition();
                break;
            case token_extern:
                HandleExtern();
                break;
            default:
                HandleTopLevelExpression();
                break;
        }
    }
}

// main driver code

int main() {
    BinaryOperatorPrecedence['<'] = 10;
    BinaryOperatorPrecedence['+'] = 20;
    BinaryOperatorPrecedence['-'] = 20;
    BinaryOperatorPrecedence['*'] = 40;

    fprintf(stderr, "ready> ");
    getNextToken();

    MainLoop();

    return 0;
}