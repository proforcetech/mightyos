#include "keyvalues.h"

#include <vector>

namespace KeyValues {

const Node *Node::child(QStringView key) const
{
    for (const Node &n : children) {
        if (QStringView(n.key).compare(key, Qt::CaseInsensitive) == 0)
            return &n;
    }
    return nullptr;
}

const Node *Node::path(std::initializer_list<QStringView> keys) const
{
    const Node *node = this;
    for (QStringView key : keys) {
        node = node->child(key);
        if (!node || !node->isBlock)
            return nullptr;
    }
    return node;
}

QString Node::valueOf(QStringView key, const QString &fallback) const
{
    for (const Node &n : children) {
        if (!n.isBlock && QStringView(n.key).compare(key, Qt::CaseInsensitive) == 0)
            return n.value;
    }
    return fallback;
}

namespace {

enum class TokenType { End, String, Open, Close, Error };

struct Token {
    TokenType type = TokenType::End;
    QString text;
    bool quoted = false;
};

class Lexer
{
public:
    Lexer(const QByteArray &data, const Limits &limits)
        : m_data(data), m_limits(limits)
    {
        if (m_data.startsWith("\xEF\xBB\xBF"))
            m_pos = 3;
    }

    Token next()
    {
        if (m_peeked) {
            m_peeked = false;
            return m_peek;
        }
        return lex();
    }

    const Token &peek()
    {
        if (!m_peeked) {
            m_peek = lex();
            m_peeked = true;
        }
        return m_peek;
    }

    QString error;

private:
    Token fail(const char *why)
    {
        error = QString::fromLatin1(why);
        return { TokenType::Error, {}, false };
    }

    void skipSpaceAndComments()
    {
        const qsizetype n = m_data.size();
        while (m_pos < n) {
            const char c = m_data.at(m_pos);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < n && m_data.at(m_pos + 1) == '/') {
                while (m_pos < n && m_data.at(m_pos) != '\n')
                    ++m_pos;
            } else {
                break;
            }
        }
    }

    Token lex()
    {
        skipSpaceAndComments();
        const qsizetype n = m_data.size();
        if (m_pos >= n)
            return { TokenType::End, {}, false };
        const char c = m_data.at(m_pos);
        if (c == '{') {
            ++m_pos;
            return { TokenType::Open, {}, false };
        }
        if (c == '}') {
            ++m_pos;
            return { TokenType::Close, {}, false };
        }
        QByteArray bytes;
        if (c == '"') {
            ++m_pos;
            for (;;) {
                if (m_pos >= n)
                    return fail("unterminated string");
                char ch = m_data.at(m_pos++);
                if (ch == '"')
                    break;
                if (ch == '\\' && m_pos < n) {
                    const char e = m_data.at(m_pos);
                    if (e == '\\' || e == '"') {
                        ch = e;
                        ++m_pos;
                    } else if (e == 'n') {
                        ch = '\n';
                        ++m_pos;
                    } else if (e == 't') {
                        ch = '\t';
                        ++m_pos;
                    }
                }
                if (ch == '\0')
                    return fail("NUL in string");
                bytes.append(ch);
                if (bytes.size() > m_limits.maxTokenLength)
                    return fail("token too long");
            }
            return { TokenType::String, QString::fromUtf8(bytes), true };
        }
        while (m_pos < n) {
            const char ch = m_data.at(m_pos);
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v'
                || ch == '{' || ch == '}' || ch == '"')
                break;
            if (ch == '\0')
                return fail("NUL in token");
            bytes.append(ch);
            ++m_pos;
            if (bytes.size() > m_limits.maxTokenLength)
                return fail("token too long");
        }
        return { TokenType::String, QString::fromUtf8(bytes), false };
    }

    const QByteArray &m_data;
    const Limits &m_limits;
    qsizetype m_pos = 0;
    Token m_peek;
    bool m_peeked = false;
};

bool isConditional(const Token &t)
{
    return t.type == TokenType::String && !t.quoted && t.text.startsWith(QLatin1Char('['))
           && t.text.endsWith(QLatin1Char(']'));
}

} // namespace

std::optional<Node> parse(const QByteArray &data, const Limits &limits, QString *error)
{
    auto fail = [error](const QString &why) -> std::optional<Node> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (data.size() > limits.maxBytes)
        return fail(QStringLiteral("file too large"));

    Lexer lexer(data, limits);
    std::vector<Node> stack;
    stack.emplace_back();
    stack.back().isBlock = true;
    int nodes = 0;

    for (;;) {
        Token t = lexer.next();
        if (t.type == TokenType::Error)
            return fail(lexer.error);
        if (t.type == TokenType::End) {
            if (stack.size() != 1)
                return fail(QStringLiteral("unexpected end of file"));
            break;
        }
        if (t.type == TokenType::Close) {
            if (stack.size() == 1)
                return fail(QStringLiteral("unbalanced '}'"));
            Node done = std::move(stack.back());
            stack.pop_back();
            stack.back().children.append(std::move(done));
            if (isConditional(lexer.peek()))
                lexer.next();
            continue;
        }
        if (t.type == TokenType::Open)
            return fail(QStringLiteral("'{' without a key"));

        if (++nodes > limits.maxNodes)
            return fail(QStringLiteral("too many nodes"));
        const QString key = t.text;
        Token v = lexer.next();
        while (isConditional(v))
            v = lexer.next();
        if (v.type == TokenType::Error)
            return fail(lexer.error);
        if (v.type == TokenType::String) {
            Node leaf;
            leaf.key = key;
            leaf.value = v.text;
            stack.back().children.append(std::move(leaf));
            if (isConditional(lexer.peek()))
                lexer.next();
        } else if (v.type == TokenType::Open) {
            if (static_cast<int>(stack.size()) > limits.maxDepth)
                return fail(QStringLiteral("nesting too deep"));
            Node block;
            block.key = key;
            block.isBlock = true;
            stack.push_back(std::move(block));
        } else {
            return fail(QStringLiteral("value expected after key"));
        }
    }
    return std::move(stack.front());
}

} // namespace KeyValues
