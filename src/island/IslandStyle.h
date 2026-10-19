#pragma once
#include <d2d1_1.h>

namespace eflow {

// Design tokens shared by the island's views (IslandWindow.cpp and one file per larger tab, such as RemindersView.cpp).
// Sizes are DIPs.

constexpr D2D1_COLOR_F Rgba(float r, float g, float b, float a) { return {r, g, b, a}; }
inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, c.a * a}; }

inline constexpr D2D1_COLOR_F kText = Rgba(0.96f, 0.96f, 0.98f, 1.0f);
inline constexpr D2D1_COLOR_F kTextDim = Rgba(0.96f, 0.96f, 0.98f, 0.52f);
inline constexpr D2D1_COLOR_F kAccent = Rgba(0.42f, 0.70f, 1.0f, 1.0f);
inline constexpr D2D1_COLOR_F kAmber = Rgba(1.0f, 0.73f, 0.30f, 1.0f);
inline constexpr D2D1_COLOR_F kRed = Rgba(1.0f, 0.42f, 0.38f, 1.0f);
inline constexpr D2D1_COLOR_F kGreen = Rgba(0.36f, 0.86f, 0.56f, 1.0f);

inline constexpr float kExpandedW = 690.0f;     // the open island (7 tabs and 4 round buttons share its top row)
inline constexpr float kExpandedH = 176.0f;     // ... its usual height (some tabs ask for more)
inline constexpr float kTabTop = 14.0f;         // the tab row
inline constexpr float kTabH = 30.0f;
inline constexpr float kMascotLane = 172.0f;    // panel width reserved on the right for the mascot

// Reminders tab (RemindersView.cpp): its hit ids, one range so IslandWindow can hand them over.
inline constexpr int kHitReminderFirst = 3500;
inline constexpr int kHitReminderRemove = 3500;   // + row: its delete button
inline constexpr int kHitReminderWhat = 3600, kHitReminderNote = 3601, kHitReminderAdd = 3602, kHitReminderMeridiem = 3603;
inline constexpr int kHitAlarmSnooze = 3610, kHitAlarmDismiss = 3611;
inline constexpr int kHitReminderLast = 3699;
inline constexpr float kRemindersH = 306.0f;    // height of the island on the Reminders tab
inline constexpr float kAlarmH = 214.0f;        // ... while a reminder rings

} // namespace eflow
