#!/usr/bin/env python3
"""Generate include/habits_data.h from the template's habits.json."""
import json, io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'LtdSaveEditorTemplateDoNotEditThisFileCopyFromItInstead', 'static', 'habits.json')
DST = os.path.join(ROOT, 'include', 'habits_data.h')

with io.open(SRC, 'r', encoding='utf-8') as f:
    raw = json.load(f)

ORDER = ['WalkType','StandType','EatType','GreetingType','GetAngryType',
         'ExpressionType','VoiceType','StomachType','OtherType']
CAT_LABEL = {
    'WalkType':'Walking','StandType':'Standing','EatType':'Eating',
    'GreetingType':'Greeting','GetAngryType':'Anger',
    'ExpressionType':'Expression','VoiceType':'Voice',
    'StomachType':'Stomach','OtherType':'Other'
}

def esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

lines = []
lines.append('#pragma once')
lines.append('// Auto-generated from habits.json by tools/gen_habits.py — DO NOT hand-edit')
lines.append('')
lines.append('struct HabitDef { const char* name; int category; const char* label; };')
lines.append('')
lines.append('static const char* const HABIT_CAT_LABEL[] = {')
for c in ORDER:
    lines.append('    "' + CAT_LABEL[c] + '",')
lines.append('};')
lines.append('static const char* const HABIT_CAT_NAME[] = {')
for c in ORDER:
    lines.append('    "' + c + '",')
lines.append('};')
lines.append('static const int HABIT_CAT_COUNT = ' + str(len(ORDER)) + ';')
lines.append('')
lines.append('static const HabitDef HABITS[] = {')
total = 0
for cat_idx, cat in enumerate(ORDER):
    items = sorted([r for r in raw if r['c'] == cat], key=lambda x: x['s'])
    lines.append('    // ' + cat)
    for it in items:
        label = it['l'].get('USen') or it['l'].get('EUen') or it['n']
        lines.append('    { "' + esc(it['n']) + '", ' + str(cat_idx) + ', "' + esc(label) + '" },')
        total += 1
lines.append('};')
lines.append('static const int HABIT_COUNT = ' + str(total) + ';')

with io.open(DST, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
print('Wrote', total, 'habits to', DST)
