#pragma once

#include <QString>
#include <utility>
#include <variant>

namespace pp {

/// Short, non-technical error carrier. `code` is for tests and diagnostics,
/// `message` is the sentence a person may read on screen.
struct Error {
    QString code;
    QString message;

    Error() = default;
    Error(QString c, QString m) : code(std::move(c)), message(std::move(m)) {}
};

inline Error error(QString code, QString message)
{
    return Error{std::move(code), std::move(message)};
}

template <typename T>
class Result
{
public:
    Result(T value) : m_value(std::move(value)) {}
    Result(Error err) : m_value(std::move(err)) {}

    bool ok() const { return std::holds_alternative<T>(m_value); }
    explicit operator bool() const { return ok(); }

    const T &value() const { return std::get<T>(m_value); }
    T &value() { return std::get<T>(m_value); }
    const T *operator->() const { return &value(); }
    T *operator->() { return &value(); }

    const Error &error() const { return std::get<Error>(m_value); }

private:
    std::variant<T, Error> m_value;
};

template <>
class Result<void>
{
public:
    Result() = default;
    Result(Error err) : m_error(std::move(err)) {}

    bool ok() const { return m_error.code.isEmpty(); }
    explicit operator bool() const { return ok(); }
    const Error &error() const { return m_error; }

private:
    Error m_error;
};

} // namespace pp
