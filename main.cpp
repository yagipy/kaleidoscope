#include "include/KaleidoscopeJIT.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

enum Token {
    token_eof = -1,

    // commands
    token_def = -2,
    token_extern = -3,

    // primary
    token_identifier = -4,
    token_number = -5,

    // control
    token_if = -6,
    token_then = -7,
    token_else = -8,
    token_for = -9,
    token_in = -10,

    // operators
    token_binary = -11,
    token_unary = -12
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
        if (IdentifierStr == "if")
            return token_if;
        if (IdentifierStr == "then")
            return token_then;
        if (IdentifierStr == "else")
            return token_else;
        if (IdentifierStr == "for")
            return token_for;
        if (IdentifierStr == "in")
            return token_in;
        if (IdentifierStr == "binary")
            return token_binary;
        if (IdentifierStr == "unary")
            return token_unary;
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

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//

// 式、プロトタイプ、関数オブジェクトが存在する
namespace {
    // 式
    class ExprAST {
    public:
        virtual ~ExprAST() = default;
        // 該当のASTノードのIRを、依存するすべてのものと一緒に返却する
        // Valueは、LLVMで"Static Single Assignment (SSA) register"または"SSA value"を表すために使われるクラス
        // SSAはその値が関連する命令の実行時に計算され、イミュータブルである(参考: https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl03.html)
        virtual Value *codegen() = 0;
    };

    // 数値リテラル
    class NumberExprAST: public ExprAST {
        double Val; // 数値
    public:
        NumberExprAST(double Val): Val(Val) {}
        Value *codegen() override;
    };

    // 変数
    class VariableExprAST: public ExprAST {
        std::string Name; // 変数名
    public:
        VariableExprAST(const std::string &Name): Name(Name) {}
        Value *codegen() override;
    };

    // 二項演算子(binary operator)
    class BinaryExprAST: public ExprAST {
        char Op; // オペコード(例: '+')
        std::unique_ptr<ExprAST> LHS, RHS;
    public:
        BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS): Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
        Value *codegen() override;
    };

    // 単項演算子(unary operator)
    class UnaryExprAST: public ExprAST {
        char Opcode;
        std::unique_ptr<ExprAST> Operand;
    public:
        UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand): Opcode(Opcode), Operand(std::move(Operand)) {}
        Value *codegen() override;
    };

    // 関数呼び出し
    class CallExprAST: public ExprAST {
        std::string Callee; // 関数名
        std::vector<std::unique_ptr<ExprAST>> Args; // 引数式
    public:
        CallExprAST(const std::string &Callee, std::vector<std::unique_ptr<ExprAST>> Args): Callee(Callee), Args(std::move(Args)) {}
        Value *codegen() override;
    };

    // if
    class IfExprAST: public ExprAST {
        std::unique_ptr<ExprAST> Condition, Then, Else;
    public:
        IfExprAST(std::unique_ptr<ExprAST> Condition, std::unique_ptr<ExprAST> Then, std::unique_ptr<ExprAST> Else): Condition(std::move(Condition)), Then(std::move(Then)), Else(std::move(Else)) {}
        Value *codegen() override;
    };

    // for
    class ForExprAST: public ExprAST {
        std::string VarName;
        std::unique_ptr<ExprAST> Start, End, Step, Body;
    public:
        ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start, std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step, std::unique_ptr<ExprAST> Body)
        : VarName(VarName), Start(std::move(Start)), End(std::move(End)), Step(std::move(Step)), Body(std::move(Body)) {}
        Value *codegen() override;
    };

    // 関数のプロトタイプ(インターフェース)
    class PrototypeAST {
        std::string Name; // 関数名
        std::vector<std::string> Args; // 引数名
        bool IsOperator;
        unsigned Precedence; // Precedence if a binary operator
    public:
        PrototypeAST(const std::string &Name, std::vector<std::string> Args, bool IsOperator = false, unsigned Precedence = 0): Name(Name), Args(std::move(Args)), IsOperator(IsOperator), Precedence(Precedence) {}
        Function *codegen();
        const std::string &getName() const { return Name; }

        bool isUnaryOperator() const { return IsOperator && Args.size() == 1; }
        bool isBinaryOperator() const { return IsOperator && Args.size() == 2; }

        char getOperatorName() const {
            assert(isUnaryOperator() || isBinaryOperator());
            return Name[Name.size() - 1];
        }

        unsigned getBinaryPrecedence() const { return Precedence; }
    };

    // 関数
    class FunctionAST {
        std::unique_ptr<PrototypeAST> Proto;
        std::unique_ptr<ExprAST> Body;
    public:
        FunctionAST(std::unique_ptr<PrototypeAST> Proto, std::unique_ptr<ExprAST> Body): Proto(std::move(Proto)), Body(std::move(Body)) {}
        Function *codegen();
    };
}

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

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

static std::unique_ptr<ExprAST> ParseIfExpr() {
    getNextToken();

    auto Condition = ParseExpression();
    if (!Condition)
        return nullptr;

    if (CurrentToken != token_then)
        return LogError("expected then");
    getNextToken();

    auto Then = ParseExpression();
    if (!Then)
        return nullptr;

    if (CurrentToken != token_else)
        return LogError("expected else");
    getNextToken();

    auto Else = ParseExpression();
    if (!Else)
        return nullptr;

    return std::make_unique<IfExprAST>(std::move(Condition), std::move(Then), std::move(Else));
}

// 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
    getNextToken();

    if (CurrentToken != token_identifier)
        return LogError("expected identifier after for");

    std::string IdName = IdentifierStr;
    getNextToken();

    if (CurrentToken != '=')
        return LogError("expected '=' after for");
    getNextToken();

    auto Start = ParseExpression();
    if (!Start)
        return nullptr;
    if (CurrentToken != ',')
        return LogError("expected ',' after for start value");
    getNextToken();

    auto End = ParseExpression();
    if (!End)
        return nullptr;

    // Stepはオプション
    // 2番目のカンマが存在するかどうかで判断
    std::unique_ptr<ExprAST> Step;
    if (CurrentToken == ',') {
        getNextToken();
        Step = ParseExpression();
        if (!Step)
            return nullptr;
    }

    if (CurrentToken != token_in)
        return LogError("expected 'in' after for");
    getNextToken();

    auto Body = ParseExpression();
    if (!Body)
        return nullptr;

    return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End), std::move(Step), std::move(Body));
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
        case token_if:
            return ParseIfExpr();
        case token_for:
            return ParseForExpr();
    }
}

static std::unique_ptr<ExprAST> ParseUnary() {
    // CurrentTokenが演算子でない場合は、PrimaryExprでなければならない
    if (!isascii(CurrentToken) || CurrentToken == '(' || CurrentToken == ',')
        return ParsePrimary();

    // 単項演算子の場合
    // 演算子をOpcodeとする
    int Opc = CurrentToken;
    getNextToken();
    if (auto Operand = ParseUnary()) // 残りの部分を単項演算子としてパースする
        return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
    return nullptr;
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

        auto RHS = ParseUnary();
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
    auto LHS = ParseUnary(); // 上記例の'a'をパース(CurrentTokenは'+'になる)
    if (!LHS)
        return nullptr;

    return ParseBinaryOperatorRHS(0, std::move(LHS));
}

// 関数のプロトタイプをパース
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    std::string FnName;

    unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary
    unsigned BinaryPrecedence = 30;

    switch (CurrentToken) {
        default:
            return LogErrorP("Expected function name in prototype");
        case token_identifier:
            FnName = IdentifierStr;
            Kind = 0;
            getNextToken();
            break;
        case token_unary:
            getNextToken();
            if (!isascii(CurrentToken))
                return LogErrorP("Expected unary operator");
            FnName = "unary";
            FnName += (char)CurrentToken;
            Kind = 1;
            getNextToken();
            break;
        case token_binary:
            getNextToken();
            if (!isascii(CurrentToken))
                return LogErrorP("Expected binary operator");
            // 二項演算子だった場合、関数名は"binary~~になる"(例: "@"演算子の場合、"binary@"のような名前を構築する)
            // LLVMのシンボルテーブルのシンボル名には、nullの埋め込みも含めて、どんな文字でも許される
            FnName = "binary";
            FnName += (char)CurrentToken;
            Kind = 2;
            getNextToken();

            if (CurrentToken == token_number) {
                if (NumVal < 1 || NumVal > 100)
                    return LogErrorP("Invalid precedence: must be 1..100");
                BinaryPrecedence = (unsigned)NumVal;
                getNextToken();
            }
            break;
    }

    if (CurrentToken != '(')
        return LogErrorP("Expected '(' in prototype");

    // 引数名のリストを読み込む
    std::vector<std::string> ArgNames;
    while (getNextToken() == token_identifier)
        ArgNames.push_back(IdentifierStr);
    if (CurrentToken != ')')
        return LogErrorP("Expected ')' in prototype");

    getNextToken();

    if (Kind && ArgNames.size() != Kind)
        return LogErrorP("Invalid number of operands for operator");

    return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames), Kind != 0, BinaryPrecedence);
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

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

// 型と定数値のテーブルのような、多くのLLVMのコアデータ構造を所有するオブジェクト
static std::unique_ptr<LLVMContext> TheContext;
// LLVMの命令を簡単に生成できるようにするヘルパーオブジェクト。IRBuilderクラスのテンプレートのインスタンスは、命令を挿入する現在の場所を追跡し、新しい命令を作成するためのメソッドを持っている
static std::unique_ptr<IRBuilder<>> Builder;
// 関数とグローバル変数を含むLLVMの構成要素。LLVM IRがコードを含むために使用する可能性の高いトップレベルの構造。codegen()メソッドがunique_ptr<Value>ではなく、生のValue*を返す理由であり、生成するすべてのIRのためのメモリを所有する
static std::unique_ptr<Module> TheModule;
// 現在のスコープでどの値が定義され、そのLLVM表現が何であるかを追跡する(コードのシンボルテーブル)。現状は関数パラメータのみ参照できる。
static std::map<std::string, Value *> NamedValues;
static std::unique_ptr<legacy::FunctionPassManager> TheFunctionPassManager;
// sinやcosを呼べる
// JITに追加されたすべてのモジュールを、新しいものから順に検索し、最新の定義を見つける
// 見つからない場合は、Kaleidoscopeプロセス自体で "dlsym("sin")" を呼び出す
// libm版のsinを直接呼び出される
static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
static ExitOnError ExitOnErr;

Value *LogErrorV(const char *Str) {
    LogError(Str);
    return nullptr;
}

// TheModuleを検索して既存の関数宣言を見つけ、見つからなければFunctionProtosから新しい宣言を生成するようにフォールバック
Function *getFunction(std::string Name) {
    if (auto *F = TheModule->getFunction(Name))
        return F;

    auto FI = FunctionProtos.find(Name);
    if (FI != FunctionProtos.end())
        return FI->second->codegen();

    return nullptr;
}

Value *NumberExprAST::codegen() {
    // 数値定数はConstantFPクラス
    // 内部でAPFloatに数値を保持します(APFloatはArbitrary Precisionの浮動小数点定数を保持できる機能を持っている)
    return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
    Value *V = NamedValues[Name];
    if (!V)
        LogErrorV("Unknown variable name");
    return V;
}

Value *UnaryExprAST::codegen() {
    Value *OperandV = Operand->codegen();
    if (!OperandV)
        return nullptr;

    Function *F = getFunction(std::string("unary") + Opcode);
    if (!F)
        return LogErrorV("Unknown unary operator");

    return Builder->CreateCall(F, OperandV, "unop");
}

Value *BinaryExprAST::codegen() {
    // それぞれ再帰的に出力
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R)
        return nullptr;

    switch (Op) {
        case '+':
            return Builder->CreateFAdd(L, R, "addtmp");
        case '-':
            return Builder->CreateFSub(L, R, "subtmp");
        case '*':
            return Builder->CreateFMul(L, R, "multmp");
        case '<':
            L = Builder->CreateFCmpULT(L, R, "cmptmp");
            // fcmp命令は常に「i1」値(1ビットの整数)を返すと規定されているが、0.0または1.0の値が欲しいので変換を行っている
            return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
        default:
            break;
    }

    // 組み込みの二項演算子でない場合、ユーザー定義の二項演算子である必要がある。それを呼び出す
    // シンボルテーブルから適切な演算子を探し、その演算子への関数呼び出しを生成する(ユーザー定義演算子は通常の関数として構築される)
    Function *F = getFunction(std::string("binary") + Op);
    assert(F && "binary operator not found!");

    Value *Ops[2] = { L, R };
    return Builder->CreateCall(F, Ops, "binop");
}

Value *CallExprAST::codegen() {
    // グローバルのModuleから取得
    Function *CalleeF = getFunction(Callee);
    if (!CalleeF)
        return LogErrorV("Unknown function referenced");

    if (CalleeF->arg_size() != Args.size())
        return LogErrorV("Incorrect # arguments passed");

    std::vector<Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
        // 各引数を再帰的にコード化
        ArgsV.push_back(Args[i]->codegen());
        if (!ArgsV.back())
            return nullptr;
    }

    return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Value *IfExprAST::codegen() {
    Value *ConditionV = Condition->codegen();
    if (!ConditionV)
        return nullptr;

    // 0.0と条件式を比較し真偽値に変換する
    ConditionV = Builder->CreateFCmpONE(ConditionV, ConstantFP::get(*TheContext, APFloat(0.0)), "ifcond");

    // 現在構築されているFunctionオブジェクトを取得
    Function *TheFunction = Builder->GetInsertBlock()->getParent();

    // TheFunctionを渡すことで、自動的に新しいブロックを指定された関数の末尾に挿入する
    BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
    // 下記2つのブロックは作成されますが、まだ関数に挿入されない
    BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    // 条件分岐を挿入
    Builder->CreateCondBr(ConditionV, ThenBB, ElseBB);

    // 「then」ブロックに挿入を開始する(指定されたブロックの末尾に挿入ポイントが移動する)
    Builder->SetInsertPoint(ThenBB);

    Value *ThenV = Then->codegen();
    if (!ThenV)
        return nullptr;

    Builder->CreateBr(MergeBB);
    // Then->codegenによってブロックが変更されてしまう可能性がある(if/then/elseのネスト等)
    // 最新を取得する必要がある
    ThenBB = Builder->GetInsertBlock();

    TheFunction->getBasicBlockList().push_back(ElseBB);
    Builder->SetInsertPoint(ElseBB);

    Value *ElseV = Else->codegen();
    if (!ElseV)
        return nullptr;

    Builder->CreateBr(MergeBB);
    ElseBB = Builder->GetInsertBlock();

    TheFunction->getBasicBlockList().push_back(MergeBB);
    Builder->SetInsertPoint(MergeBB);
    PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");

    // PHIのブロックと値のペアをセットアップ
    PN->addIncoming(ThenV, ThenBB);
    PN->addIncoming(ElseV, ElseBB);
    return PN;
}

Value *ForExprAST::codegen() {
    Value *StartVal = Start->codegen();
    if (!StartVal)
        return nullptr;

    // ループヘッダ用の新しいブロックを作成し、挿入する
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *PreHeaderBB = Builder->GetInsertBlock();
    BasicBlock *LoopBB = BasicBlock::Create(*TheContext, "loop", TheFunction);

    // 現在のブロックからLoopBBへの明示的なフォールスルーを挿入する
    Builder->CreateBr(LoopBB);

    Builder->SetInsertPoint(LoopBB);

    // ループ誘導変数用のPHIノードを作成する
    PHINode *Variable = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, VarName);
    Variable->addIncoming(StartVal, PreHeaderBB);

    // シンボルテーブルに変数を導入(シャドーイング)
    Value *OldVal = NamedValues[VarName];
    NamedValues[VarName] = Variable;

    // Bodyのコード化
    if (!Body->codegen())
        return nullptr;

    // Stepのコード化
    Value *StepVal = nullptr;
    if (Step) {
        StepVal = Step->codegen();
        if (!StepVal)
            return nullptr;
    } else {
        StepVal = ConstantFP::get(*TheContext, APFloat(1.0));
    }

    // ループの次の繰り返しにおけるループ変数の値
    Value *NextVar = Builder->CreateFAdd(Variable, StepVal, "nextvar");

    Value *EndCondition = End->codegen();
    if (!EndCondition)
        return nullptr;

    // ループの終了値を評価
    // if/then/else文の条件評価と同じ
    EndCondition = Builder->CreateFCmpONE(EndCondition, ConstantFP::get(*TheContext, APFloat(0.0)), "loopcond");

    BasicBlock *LoopEndBB = Builder->GetInsertBlock();
    BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop", TheFunction);

    // 終了条件の値に基づいて、ループの再実行とループの終了のどちらかを選択する条件分岐を作成する
    Builder->CreateCondBr(EndCondition, LoopBB, AfterBB);
    Builder->SetInsertPoint(AfterBB);

    Variable->addIncoming(NextVar, LoopEndBB);

    // ループ変数をシンボルテーブルから削除
    if (OldVal)
        NamedValues[VarName] = OldVal;
    else
        NamedValues.erase(VarName);

    // forループのコード生成は常に0.0を返す
    return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

Function *PrototypeAST::codegen() {
    // LLVM double型のN個のベクトルを作成する
    std::vector<Type*> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
    // 与えられたプロトタイプに対して使用されるべきFunctionTypeを作成する。false=可変長引数ではない。型は定数なのでgetになる
    FunctionType *FT = FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);
    // プロトタイプに対応するIR Functionを作成する。使用する型、リンク、名前と、どのモジュールに挿入するかを示す
    // ExternalLinkage=関数が現在のモジュールの外部で定義され、かつ、モジュールの外部の関数から呼び出される可能性があることを意味する
    // TheModuleのシンボルテーブルに登録される
    Function *F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    // 各引数の名前を、Prototypeで与えられた名前に従って設定する
    unsigned Idx = 0;
    for (auto &Arg: F->args())
        Arg.setName(Args[Idx++]);

    return F;
}

Function *FunctionAST::codegen() {
    auto &P = *Proto;
    FunctionProtos[Proto->getName()] = std::move(Proto);
    Function *TheFunction = getFunction(P.getName());

//    if (!TheFunction) // 以前のバージョンが存在しない場合
//        TheFunction = Proto->codegen();

    if (!TheFunction)
        return nullptr;

//    if (!TheFunction->empty())
//        return (Function *)LogErrorV("Function cannot be redefined");

    // ユーザー定義演算子の場合、優先順位表に登録する
    if (P.isBinaryOperator())
        BinaryOperatorPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();

    BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
    Builder->SetInsertPoint(BB);

    // NamedValuesに引数を保存(VariableExprASTノードからアクセスできるようにする)
    NamedValues.clear();
    for (auto &Arg: TheFunction->args())
        NamedValues[std::string(Arg.getName())] = &Arg;

    if (Value *RetVal = Body->codegen()) {
        Builder->CreateRet(RetVal);

        // 生成されたコードに対して様々な整合性チェックを行い、コンパイラが正しく動作しているかどうかを判断する
        verifyFunction(*TheFunction);

        // 関数の最適化
        TheFunctionPassManager->run(*TheFunction);

        return TheFunction;
    }

    // エラー処理
    TheFunction->eraseFromParent();
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

static void InitializeModuleAndPassManager() {
    TheContext = std::make_unique<LLVMContext>();
    TheModule = std::make_unique<Module>("my cool jit", *TheContext);
    TheModule->setDataLayout(TheJIT->getDataLayout());

    TheFunctionPassManager = std::make_unique<legacy::FunctionPassManager>(TheModule.get());
    TheFunctionPassManager->add(createInstructionCombiningPass());
    TheFunctionPassManager->add(createReassociatePass());
    TheFunctionPassManager->add(createGVNPass());
    TheFunctionPassManager->add(createCFGSimplificationPass());
    TheFunctionPassManager->doInitialization();

    Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Read function definition:\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            ExitOnErr(TheJIT->addModule(ThreadSafeModule(std::move(TheModule), std::move(TheContext))));
            InitializeModuleAndPassManager();
        }
    } else {
        getNextToken();
    }
}

static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Read extern:\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
        }
    } else {
        getNextToken();
    }
}

static void HandleTopLevelExpression() {
    if (auto FnAST = ParseTopLevelExpr()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Read top level expression:\n");
            FnIR->print(errs());
            fprintf(stderr, "\n");

            auto RT = TheJIT->getMainJITDylib().createResourceTracker();

            auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
            ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
            // 後続のコードを格納する新しいモジュールを追加する
            InitializeModuleAndPassManager();

            // 最終的に生成されるコードへのポインタを取得
            auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));

            // 関数のメモリ内アドレスを取得
            double (*FP)() = (double (*)())(intptr_t)ExprSymbol.getAddress();
            // 結果ポインタをその型の関数ポインタにキャストして直接呼び出すだけで良い(JITコンパイルされたコードと、アプリケーションに静的にリンクされたネイティブのマシンコードとの間に差はない)
            fprintf(stderr, "Evaluated to %f\n", FP());

            ExitOnErr(RT->remove());
        }
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

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

extern "C" DLLEXPORT double putchard(double X) {
    fputc((char)X, stderr);
    return 0;
}

extern "C" DLLEXPORT double printd(double X) {
    fprintf(stderr, "%f\n", X);
    return 0;
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main(int argc, char *argv[]) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    BinaryOperatorPrecedence['<'] = 10;
    BinaryOperatorPrecedence['+'] = 20;
    BinaryOperatorPrecedence['-'] = 20;
    BinaryOperatorPrecedence['*'] = 40;

    fprintf(stderr, "ready> ");
    getNextToken();

    TheJIT = ExitOnErr(KaleidoscopeJIT::Create());

    InitializeModuleAndPassManager();

    MainLoop();

    TheModule->print(errs(), nullptr);

    return 0;
}
