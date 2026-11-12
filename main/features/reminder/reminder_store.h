#ifndef REMINDER_STORE_H
#define REMINDER_STORE_H

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

// One-shot reminders that live on the device only. They are independent of the
// network, so they still fire when the device has no connection.
namespace reminder {

struct Item {
    std::string text;
    time_t due = 0;
};

// Creates a reminder `minutes` from now (1..10080).
bool AddIn(int minutes, const std::string& text, Item& added, std::string& error);

// Creates a reminder at a wall-clock moment. `clock` is "HH:MM"; an empty clock
// means 09:00, so "напомни завтра" needs no time. `day` is empty (today, or
// tomorrow when that time already passed), "today", "tomorrow", "послезавтра",
// "13.11", "13.11.2026" or "2026-11-13".
bool AddAt(const std::string& day, const std::string& clock, const std::string& text,
           Item& added, std::string& error);

// The waiting reminders in the order they are shown and indexed.
bool List(std::vector<Item>& items);

// Removes the reminder at `index` of List().
bool Remove(size_t index, std::string& error);

// Removes the first reminder whose text contains `match`, ignoring case; the
// removed reminder is returned so the assistant can repeat what it cancelled.
bool RemoveMatching(const std::string& match, Item& removed, std::string& error);

// Removes every reminder and returns how many there were.
int RemoveAll();

// "22:40", "завтра в 08:00" or "14.11 в 09:15".
std::string WhenText(time_t due);

// Confirmation for the assistant, e.g. "Напомню в 22:40: выключить машину."
std::string DescribeItem(const Item& item);

// Upcoming reminders, soonest first; says so when there are none.
std::string Describe(int max_items);

// Removes and returns the reminders that are due at `now`.
bool TakeDue(time_t now, std::vector<Item>& due);

}  // namespace reminder

#endif  // REMINDER_STORE_H
