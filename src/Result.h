#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>
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

    /// Only valid on a successful result; a failed result has no value.
    const T &value() const
    {
        Q_ASSERT_X(ok(), "Result::value", "called on a failed result");
        return std::get<T>(m_value);
    }
    T &value()
    {
        Q_ASSERT_X(ok(), "Result::value", "called on a failed result");
        return std::get<T>(m_value);
    }
    const T *operator->() const { return &value(); }
    T *operator->() { return &value(); }

    /// Only valid on a failed result; a successful result carries no error.
    /// Misuse asserts in a debug build and throws otherwise, exactly like
    /// Result<void>::error().
    const Error &error() const
    {
        Q_ASSERT_X(!ok(), "Result::error", "called on a successful result");
        return std::get<Error>(m_value);
    }

private:
    std::variant<T, Error> m_value;
};

/// A result that only carries failure information. Success is the absence of an
/// error, not an empty error code: every Error, even one without a code, means
/// failure, so a message can never be swallowed by an "empty code is fine" rule.
template <>
class Result<void>
{
public:
    Result() = default;
    Result(Error err) : m_error(std::move(err)) {}

    bool ok() const { return !m_error.has_value(); }
    explicit operator bool() const { return ok(); }

    /// Only valid on a failed result. Misuse asserts in a debug build; with
    /// assertions compiled out it throws std::bad_optional_access, the same
    /// contract Result<T>::error() has through std::get.
    const Error &error() const
    {
        Q_ASSERT_X(!ok(), "Result::error", "called on a successful result");
        return m_error.value();
    }

private:
    std::optional<Error> m_error;
};

} // namespace pp
