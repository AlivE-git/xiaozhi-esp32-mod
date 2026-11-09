#ifndef CURRENCY_CONVERTER_H
#define CURRENCY_CONVERTER_H

#include <string>

// Converts money between currencies using the daily reference rates of the
// Russian Central Bank, published as JSON by the open cbr-xml-daily.ru mirror
// (the official site only offers XML over plain HTTP).
//
// The rates are quoted against the ruble, so every pair is converted through it,
// including pairs that do not involve the ruble at all. About fifty currencies
// are covered; a few of them (KZT, JPY, UAH...) are quoted for a bundle of units
// and that nominal is divided out.
//
// The request is HTTPS and can take a few seconds, so call this from a tool
// handler or the web server task, never from the audio pipeline.
class CurrencyConverter {
public:
    struct Rate {
        std::string from;        // ISO 4217 code, e.g. "USD"
        std::string to;          // ISO 4217 code, e.g. "RUB"
        double amount = 1.0;     // converted amount expressed in `from`
        double rate = 0.0;       // how many `to` one `from` costs
        double converted = 0.0;  // amount * rate
        std::string date;        // rate date as DD.MM.YYYY
        std::string source;      // human readable source name
    };

    // Converts `amount` of `from` into `to`. On failure returns false and fills
    // `error` with a short Russian message.
    static bool Convert(const std::string& from, const std::string& to, double amount, Rate& rate,
                        std::string& error);

    // Spoken-style summary for the assistant; read it to the user as it is.
    static std::string Describe(const Rate& rate);
};

#endif  // CURRENCY_CONVERTER_H
