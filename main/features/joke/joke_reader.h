#ifndef JOKE_READER_H
#define JOKE_READER_H

#include <string>

// Fetches one random joke from rzhunemogu.ru (category "Анекдоты", CType=1).
//
// The API is HTTPS-only and the request blocks the calling task for up to a few
// seconds, so call it from the MCP or HTTP server task, never from the main
// event loop or audio tasks.
class JokeReader {
public:
    // Returns the joke text. On failure returns false and fills `error` with a
    // short Russian message.
    static bool Fetch(std::string& joke, std::string& error);

    // The joke as plain text for the assistant to read aloud; on failure returns
    // a spoken-style error message instead.
    static std::string Tell();
};

#endif  // JOKE_READER_H
