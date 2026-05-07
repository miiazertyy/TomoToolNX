#pragma once
// Auto-generated from habits.json by tools/gen_habits.py — DO NOT hand-edit

struct HabitDef { const char* name; int category; const char* label; };

static const char* const HABIT_CAT_LABEL[] = {
    "Walking",
    "Standing",
    "Eating",
    "Greeting",
    "Anger",
    "Expression",
    "Voice",
    "Stomach",
    "Other",
};
static const char* const HABIT_CAT_NAME[] = {
    "WalkType",
    "StandType",
    "EatType",
    "GreetingType",
    "GetAngryType",
    "ExpressionType",
    "VoiceType",
    "StomachType",
    "OtherType",
};
static const int HABIT_CAT_COUNT = 9;

static const HabitDef HABITS[] = {
    // WalkType
    { "WalkSwagger", 0, "walks with a swagger" },
    { "WalkToeIn", 0, "walks cutely" },
    { "WalkToeOut", 0, "walks like a rodeo rider" },
    { "WalkStoop", 0, "walks leaning forward" },
    { "WalkRhythm", 0, "walks with a rhythm" },
    { "WalkScurry", 0, "walks with tiny steps" },
    { "WalkLikeModel", 0, "walks like a model" },
    { "WalkUneasily", 0, "walks nervously" },
    { "WalkLikeRobot", 0, "walks like a robot" },
    { "WalkNoSwinging", 0, "walks without swinging arms" },
    { "WalkUnsteady", 0, "walks by bounding" },
    { "WalkFloat", 0, "floats instead of walking" },
    // StandType
    { "StandCute", 1, "stands cutely" },
    { "OftenCrossArms", 1, "stands with arms crossed" },
    { "StandProudly", 1, "stands proudly" },
    { "StandHandsFront", 1, "stands with hands folded" },
    { "StandAffected", 1, "stands smugly" },
    { "StandShy", 1, "stands shyly" },
    { "StandToeOut", 1, "stands like a rodeo rider" },
    { "StandRestless", 1, "stands restlessly" },
    { "StandRhythm", 1, "stands moving to the rhythm" },
    { "StandStraight", 1, "stands at attention" },
    { "StandShakeHip", 1, "stands while shaking hips" },
    { "StandLeanForward", 1, "stands leaning forward" },
    { "StandSweatLot", 1, "stands while wiping sweat" },
    { "StandPutsGlasses", 1, "stands while adjusting glasses" },
    // EatType
    { "EatWild", 2, "eats with gusto" },
    { "EatElegantly", 2, "eats gracefully" },
    { "EatMouthful", 2, "eats voraciously" },
    { "EatCrisp", 2, "eats quickly" },
    { "EatSmell", 2, "eats while savoring" },
    { "EatCarefully", 2, "eats cautiously" },
    { "EatCute", 2, "eats cutely" },
    { "EatSecretly", 2, "eats shyly" },
    // GreetingType
    { "GreetingTypeCackle", 3, "greets eagerly" },
    { "GreetingTypeBig", 3, "greets proudly" },
    { "GreetingVitality", 3, "greets energetically" },
    { "GreetingShy", 3, "greets shyly" },
    { "GreetingTypeShallow", 3, "greets flirtatiously" },
    { "GreetingLazy", 3, "greets listlessly" },
    { "GreetingMacho", 3, "greets confidently" },
    { "GreetingBow", 3, "greets with a forward bow" },
    { "GreetingAnnoying", 3, "greets in a hyped-up style" },
    { "GreetingHeadOnly", 3, "greets with a nod" },
    { "GreetingCurtsy", 3, "greets with a curtsy" },
    { "GreetingNoble", 3, "greets with a sweeping bow" },
    { "GreetingOsu", 3, "greets karate-style" },
    { "GreetingTypeNoting", 3, "won't greet others" },
    // GetAngryType
    { "GetAngryTypeHard", 4, "flips out when angry" },
    { "GetAngryTypeRegretfulTears", 4, "cries when angry" },
    { "GetAngryTypeBrightly", 4, "smiles when angry" },
    // ExpressionType
    { "FaceSmug", 5, "smug" },
    { "FaceRaiseEyebrows", 5, "raised eyebrows" },
    { "FaceSmile", 5, "smiley" },
    { "FaceLousy", 5, "unimpressed" },
    { "FaceOpenEyes", 5, "wide-eyed" },
    { "FaceCloseEyes", 5, "closed eyes" },
    { "FaceWink", 5, "winking" },
    { "ExpressionLess", 5, "nonchalant" },
    { "FaceBliss", 5, "blissful" },
    // VoiceType
    { "VoiceLoud", 6, "loud voice" },
    { "VoiceSmall", 6, "quiet voice" },
    { "VoiceShiny", 6, "radiant voice" },
    { "VoiceGloomy", 6, "creepy voice" },
    // StomachType
    { "StomachBig", 7, "big eater" },
    { "StomachSmall", 7, "light eater" },
    // OtherType
    { "BreakGas", 8, "public farter" },
    { "Chicken", 8, "scaredy-cat" },
    { "SnoreLoudly", 8, "snores loudly" },
    { "LifeTimeNightOwl", 8, "night owl" },
    { "UntidySleeper", 8, "sleeps restlessly" },
    { "ChangeCloth", 8, "fashionista" },
    { "TakeItOutOnObject", 8, "throws tantrums" },
    { "BlinkLot", 8, "blinks a lot" },
};
static const int HABIT_COUNT = 74;
