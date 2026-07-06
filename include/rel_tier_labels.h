#pragma once
// Auto-generated from ltd-save-editor-2.13.0/static/mii-labels.json
// (subRelations → USen). Sub-relationship tier labels — used by
// main.cpp to map a (relationship-type-prefix, meter, isFight) tuple
// to a short tier blurb like "Soulmates" / "Not getting along" next
// to the meter readout. Strings copied verbatim from the reference
// editor so on-Switch labels match what a Mii's in-game status card
// reads.
//
// Tier index 0..6 derived via 2.13.0 universal RANK_THRESHOLDS:
//   [11, 41, 81, 120, 160, 190]
// meter < 11 -> 0,  < 41 -> 1,  < 81 -> 2,  < 120 -> 3,
// < 160 -> 4,  < 190 -> 5,  else -> 6.

inline constexpr const char* REL_TIER_COUPLE[7] = {
    "Not getting along",
    "Making it work",
    "Happy",
    "In love",
    "Very in love",
    "Super in love",
    "Soulmates",
};

inline constexpr const char* REL_TIER_COUPLE_FIGHT[7] = {
    "Still trying?",
    "Making it work?",
    "Still happy?",
    "Still in love?",
    "Still very in love?",
    "Still super in love?",
    "Still soulmates?",
};

inline constexpr const char* REL_TIER_FRIEND[7] = {
    "Not getting along",
    "Kinda getting along",
    "Friends",
    "Good friends",
    "Great friends",
    "Best friends",
    "Ultra friends",
};

inline constexpr const char* REL_TIER_FRIEND_FIGHT[7] = {
    "Not getting along?",
    "Kinda getting along?",
    "Still friends?",
    "Still good friends?",
    "Still great friends?",
    "Still best friends?",
    "Still ultra friends?",
};

inline constexpr const char* REL_TIER_KNOW[7] = {
    "Not interested",
    "Indifferent",
    "Some interest",
    "Getting familiar",
    "Seems like-minded",
    "Vibes with",
    "Wants to be friends",
};

inline constexpr const char* REL_TIER_LOVER[7] = {
    "Not getting along",
    "Kinda getting along",
    "Really likes",
    "Falling in love",
    "In love",
    "Super in love",
    "Wants to marry!",
};

inline constexpr const char* REL_TIER_LOVER_FIGHT[7] = {
    "Not getting along?",
    "Kinda getting along?",
    "Still really likes?",
    "Still falling in love?",
    "Still in love?",
    "Still super in love?",
    "Still wants to marry?",
};

inline constexpr const char* REL_TIER_ONESIDELOVE[7] = {
    "Giving up",
    "Close to giving up",
    "Has some hope",
    "Likes a lot",
    "Crushing",
    "Head over heels",
    "Ready to risk it all",
};

inline constexpr const char* REL_TIER_ONESIDELOVE_FIGHT[7] = {
    "Giving up?",
    "Close to giving up?",
    "Still interested?",
    "Still likes?",
    "Still crushing?",
    "Still head over heels?",
    "Still ready to risk it all?",
};

inline constexpr const char* REL_TIER_FAMILY[7] = {
    "Not getting along",
    "Kinda getting along",
    "Getting along well",
    "Cares for",
    "Cares a lot for",
    "Cares deeply for",
    "Cherishes",
};

inline constexpr const char* REL_TIER_DIVORCE[7] = {
    "Enemies",
    "Tries to avoid",
    "Not speaking",
    "No hard feelings",
    "On good terms",
    "Still great friends",
    "Would try again",
};

inline constexpr const char* REL_TIER_EXFRIEND[7] = {
    "Enemies",
    "Tries to avoid",
    "Not speaking",
    "Strained",
    "It's complicated",
    "Thinks about often",
    "Hopes to make up",
};

inline constexpr const char* REL_TIER_EXLOVER[7] = {
    "Enemies",
    "Tries to avoid",
    "Not speaking",
    "No hard feelings",
    "On good terms",
    "Still great friends",
    "Would try again",
};

