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

inline constexpr float kExpandedW = 770.0f;     // the open island (8 tabs and 4 round buttons share its top row)
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

// SafeShare tab (SafeShareView.cpp): its hit ids, one range so IslandWindow can hand them over.
inline constexpr int kHitShareFirst = 3700;
inline constexpr int kHitShareModeEmail = 3700, kHitShareModePhone = 3701, kHitShareSentList = 3702, kHitShareBack = 3703;
inline constexpr int kHitShareAddFiles = 3704, kHitShareClearFiles = 3705;
inline constexpr int kHitShareName = 3706, kHitShareEmail = 3707, kHitSharePassword = 3708, kHitShareReveal = 3709;
inline constexpr int kHitShareGenerate = 3710, kHitShareSend = 3711, kHitShareCancel = 3712, kHitShareDone = 3713;
inline constexpr int kHitShareCopyPassword = 3714, kHitShareCopyLink = 3715;
inline constexpr int kHitShareDevices = 3716;                 // + 0..2: 1, 3 or 5 devices
inline constexpr int kHitSharePhoneStart = 3720, kHitSharePhoneStop = 3721, kHitSharePhoneAllow = 3722, kHitSharePhoneDeny = 3723;
inline constexpr int kHitSharePhoneAnyway = 3724;          // "It's my Wi-Fi": start on a network Windows calls public
inline constexpr int kHitShareFileRemove = 3730;              // + file chip shown
inline constexpr int kHitShareRowCopy = 3800;                 // + sent row shown: copy its link
inline constexpr int kHitShareRowDelete = 3850;               // + sent row shown: delete the room now
inline constexpr int kHitShareLast = 3899;
inline constexpr float kShareH = 300.0f;        // height of the island on the SafeShare tab
inline constexpr UINT_PTR kShareTimer = 20;     // repaints the tab when a short note or a delete confirmation runs out

// The welcome tour (TourView.cpp): its hit ids, one range so IslandWindow can hand them over.
inline constexpr int kHitTourFirst = 3900;
inline constexpr int kHitTourPrimary = 3900, kHitTourSecondary = 3901, kHitTourBack = 3902, kHitTourSkip = 3903;
inline constexpr int kHitTourSwitch = 3904;                 // "Start Eflow with Windows"
inline constexpr int kHitTourDot = 3910;                    // + step: jump back to a step already seen
inline constexpr int kHitTourModel = 3920;                  // + row: which brain Solu should download
inline constexpr int kTourModelRows = 6;
inline constexpr int kHitTourLast = 3949;
inline constexpr float kTourH = 286.0f;          // height of the island during the tour
inline constexpr float kTourSoluH = 342.0f;      // ...and on the Solu step, where the list of brains needs the room
inline constexpr float kTourInviteW = 318.0f;    // the closed island while it invites the user to open it
inline constexpr UINT_PTR kTourTimer = 21;       // the last step's goodbye, before the island closes
inline constexpr int kHitSoluSetup = 3950;       // the Solu tab's "Set up Solu" row, before it is installed

} // namespace eflow
