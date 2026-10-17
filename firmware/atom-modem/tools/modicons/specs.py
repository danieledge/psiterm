"""The Modern icon pack's designs (read by tools/modicons.py).

Design language: modern, flat and bright but still the 5mx's own pictures,
drawn from their shapes; one light from the top left; a 1-pixel outline in
deep slate #2b3445 (never pure black); flat fills, no dithers; drop shadows
left to the theme's greys (material "none").

MATERIALS: name -> {'base': '#rrggbb'} (a ramp of seven steps is made,
lightest first, the base at step 3) or {'ramp': [seven colours]}; optional
'shift' (steps darker, or lighter if negative, for every level), 'levels'
({level: step}, overriding), 'line' (the colour of inside lines next to it).
Levels are the 5mx greys 15 (white), 10, 5, 0 (black) and the flattened
dithers 12.5, 7.5, 2.5; as standard 15 -> step 1, 12.5 -> 2, 10 -> 3,
7.5 and 5 -> 4, 2.5 -> 5, 0 -> 6.

ICONS: picture name (as in s7icons.json) -> design:
  'base':    material for fill no region claims (standard 'paper')
  'regions': a list, in order (the first to claim a pixel keeps it):
     ('seed', x, y, material[, {'levels': [...], 'box': (x0, y0, x1, y1)}])
        flood from x,y over 4-connected pixels of the same kind (fill, or
        ink = solid black), only of those levels, only inside box
     ('box', x0, y0, x1, y1, material[, {'ink': True, 'levels': [...], 'force': True}])
        every pixel in the box not yet claimed (fill only unless ink)
  'px':  [(x, y, colour)], explicit pixels last (colour None: not painted)
  'art': [(first row, [strings])] with 'key': {char: colour}, likewise
Colours are '#rrggbb', a NAMED colour, or material+step ('teal2').
"""

NAMED = {
    'navy': '#1f3a5f', 'teal': '#14b8a6', 'tealdark': '#0f766e', 'teallight': '#7dd3c8',
    'coral': '#f97362', 'amber': '#f5b942', 'sky': '#4aa3df', 'leaf': '#5bbf6a',
    'plum': '#8b6cc8', 'paper': '#ffffff', 'offwhite': '#fbfaf7', 'grey1': '#e4e7ec',
    'grey2': '#c5cfdc', 'mid': '#6b7385', 'slate': '#2b3445',
}

MATERIALS = {
    # neutrals: paper (documents, white parts) and metal (device bodies)
    'paper': {'ramp': ['#ffffff', '#ffffff', '#eef1f5', '#e4e7ec', '#c5cfdc', '#9aa4b5', '#6b7385']},
    'metal': {'ramp': ['#eef1f5', '#e4e7ec', '#c5cfdc', '#9aa4b5', '#6b7385', '#4a5468', '#2b3445']},
    'dark': {'ramp': ['#9aa4b5', '#6b7385', '#55607a', '#4a5468', '#3a4356', '#2b3445', '#222a38']},
    'navy': {'base': '#1f3a5f'},
    'teal': {'base': '#14b8a6'},
    'coral': {'base': '#f97362'},
    'amber': {'base': '#f5b942'},
    'sky': {'base': '#4aa3df'},
    'leaf': {'base': '#5bbf6a'},
    'plum': {'base': '#8b6cc8'},
}

ICONS = {}

MATERIALS.update({
    'folder': {'base': '#f5b942', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'arrow': {'base': '#14b8a6', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'tick': {'base': '#14b8a6', 'levels': {0: 4, 2.5: 4, 5: 3, 7.5: 3, 10: 2, 15: 1}},
    # text and marks on paper: sky blue to navy
    'text': {'ramp': ['#e8f3fb', '#cfe6f7', '#9ccbef', '#4aa3df', '#2f6fae', '#1f3a5f', '#162a45'],
             'levels': {15: 0, 12.5: 2, 10: 2, 7.5: 3, 5: 4, 2.5: 5, 0: 6}},
    # 3D solids: the lit top light, the side the base, the front darker
    'solidcoral': {'base': '#f97362', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'solidamber': {'base': '#f5b942', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'solidteal': {'base': '#14b8a6', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'solidleaf': {'base': '#5bbf6a', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'solidsky': {'base': '#4aa3df', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'solidnavy': {'base': '#1f3a5f', 'levels': {15: 0, 12.5: 1, 10: 1, 7.5: 2, 5: 3, 2.5: 4, 0: 5}},
    'solidplum': {'base': '#8b6cc8', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'card': {'base': '#4aa3df', 'levels': {15: 0, 12.5: 1, 10: 1, 7.5: 2, 5: 3, 2.5: 4, 0: 5}},
    'panel': {'base': '#4aa3df', 'levels': {15: 0, 12.5: 1, 10: 1, 7.5: 2, 5: 3, 2.5: 4, 0: 5}},
    'qmark': {'base': '#14b8a6', 'levels': {0: 4, 2.5: 4, 5: 3, 7.5: 3, 10: 2, 15: 1}},
})

SH = 'System/Apps/Shell/Shell.mbm#'
EK = 'System/Data/Eikon.mbm#'
ICONS.update({
    # New file: a page with a tick
    SH + '2': {'regions': [('seed', 13, 11, 'tick', {'box': (6, 6, 17, 18)})]},
    # New folder: a folder with a tick
    SH + '4': {'regions': [('box', 6, 7, 14, 15, 'tick', {'ink': True, 'levels': [0]}),
                           ('seed', 6, 14, 'folder')]},
    # Recent files: pages and a clock
    SH + '6': {'regions': [('seed', 18, 3, 'folder', {'levels': [15]}),
                           ('box', 0, 6, 23, 23, 'text', {'levels': [5, 7.5, 10]})]},
    # Control panel
    SH + '8': {'regions': [('box', 3, 0, 19, 23, 'panel', {'levels': [10, 12.5, 15]})]},
    # a file of unknown type: a page with a question mark
    SH + '10': {'regions': [('seed', 2, 13, 'none', {'levels': [10]}), ('box', 8, 1, 19, 9, 'qmark', {'ink': True}), ('box', 11, 10, 16, 18, 'qmark', {'ink': True}),
                            ('box', 0, 0, 23, 23, 'text', {'levels': [5, 7.5, 10]})]},
    # a folder
    SH + '16': {'regions': [('seed', 8, 10, 'folder')]},
    # the folder above (open, with an arrow)
    SH + '14': {'regions': [('seed', 5, 26, 'none', {'levels': [10]}), ('box', 17, 3, 38, 25, 'qmark', {'ink': True, 'levels': [0, 2.5, 10, 12.5]}),
                            ('box', 19, 27, 31, 34, 'qmark', {'ink': True, 'levels': [0, 2.5, 10, 12.5]}),
                            ('box', 0, 0, 47, 47, 'text', {'levels': [5, 7.5, 10]})]},
    SH + '12': {'regions': [('seed', 3, 17, 'none', {'levels': [10]}), ('box', 11, 2, 27, 15, 'qmark', {'ink': True, 'levels': [0, 2.5, 10, 12.5]}),
                            ('box', 14, 19, 23, 24, 'qmark', {'ink': True, 'levels': [0, 2.5, 10, 12.5]}),
                            ('box', 0, 0, 31, 31, 'text', {'levels': [5, 7.5, 10]})]},
    SH + '18': {'regions': [('seed', 10, 15, 'folder')]},
    SH + '20': {'regions': [('seed', 15, 25, 'folder')]},
    SH + '24': {'regions': [('seed', 20, 25, 'arrow'), ('seed', 10, 12, 'folder')]},
    SH + '26': {'regions': [('seed', 30, 36, 'arrow'), ('seed', 15, 20, 'folder')]},
    EK + '4': {'regions': [('seed', 12, 9, 'arrow', {'levels': [5]}), ('seed', 8, 5, 'folder', {'levels': [10]})]},
    SH + '22': {'regions': [('seed', 14, 19, 'arrow'), ('seed', 8, 8, 'folder')]},
    EK + '2': {'regions': [('seed', 8, 8, 'folder', {'levels': [5]})]},
    EK + '3': {'regions': [('seed', 8, 7, 'folder', {'levels': [5]})]},
})

AP = 'System/Apps/'
ICONS.update({
    # Word: a page of text
    AP + 'Word/Word.aif#48x48': {'regions': [('seed', 1, 17, 'none', {'levels': [10]}), ('box', 6, 2, 40, 40, 'text', {'levels': [5, 7.5]})]},
    # Data: a card index
    AP + 'Data/Data.aif#48x48': {'regions': [('seed', 20, 40, 'solidteal'), ('seed', 3, 22, 'solidteal'),
                                             ('seed', 10, 8, 'card', {'levels': [10]}), ('seed', 37, 15, 'card', {'levels': [10]})]},
    # Agenda: an open diary
    AP + 'Agenda/Agenda.aif#48x48': {'regions': [('seed', 1, 19, 'solidcoral', {'levels': [5, 2.5]}), ('seed', 11, 40, 'solidcoral', {'levels': [5, 2.5]}),
                                                 ('seed', 44, 27, 'solidcoral', {'levels': [5, 2.5]}), ('seed', 18, 44, 'solidcoral', {'levels': [5, 2.5]}),
                                                 ('seed', 38, 31, 'solidcoral', {'levels': [5, 2.5]}),
                                                 ('box', 8, 0, 46, 40, 'text', {'levels': [5]})]},
    # Sheet: a grid and a bar chart
    AP + 'Sheet/Sheet.aif#48x48': {'regions': [('seed', 10, 30, 'solidcoral'), ('seed', 18, 25, 'solidamber'),
                                               ('seed', 26, 20, 'solidleaf'), ('seed', 34, 15, 'solidsky')]},
})

MATERIALS.update({
    'logocoral': {'base': '#f97362', 'levels': {0: 3, 2.5: 4, 5: 4, 7.5: 2, 10: 2, 12.5: 1, 15: 1}},
    'logoleaf': {'base': '#5bbf6a', 'levels': {0: 3, 2.5: 4, 5: 4, 7.5: 2, 10: 2, 12.5: 1, 15: 1}},
    'logosky': {'base': '#4aa3df', 'levels': {0: 3, 2.5: 4, 5: 4, 7.5: 2, 10: 2, 12.5: 1, 15: 1}},
    'ocean': {'base': '#4aa3df', 'levels': {15: 1, 12.5: 2, 10: 3, 7.5: 3, 5: 4, 2.5: 5, 0: 6}},
    'land': {'base': '#5bbf6a', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'clockface': {'base': '#f5b942', 'levels': {15: 0, 12.5: 0, 10: 1, 7.5: 2, 5: 3, 2.5: 4, 0: 5}},
})

ICONS.update({
    # Sketch: a pad, a drawing and a pen
    AP + 'Paint/Paint.aif#48x48': {'regions': [
        ('box', 32, 0, 47, 13, 'solidcoral', {'levels': [10, 7.5, 15]}),
        ('box', 0, 10, 16, 46, 'solidnavy', {'levels': [5, 10, 2.5]}),
        ('box', 24, 23, 34, 29, 'solidamber', {'levels': [12.5]}),
        ('box', 18, 18, 47, 35, 'solidleaf', {'levels': [5, 2.5]})]},
    # Jotter: a spiral pad and a pencil
    AP + 'Jotter/Jotter.aif#48x48': {'regions': [
        ('box', 40, 4, 47, 8, 'solidcoral', {'levels': [10, 15]}),
        ('box', 27, 4, 47, 24, 'solidamber', {'levels': [7.5, 10, 2.5, 5]}),
        ('box', 5, 0, 34, 27, 'text', {'levels': [10]})]},
    # Spell: a book with "Abc" on its cover
    AP + 'Spell/Spell.aif#48x48': {'regions': [
        ('box', 10, 9, 16, 43, 'solidcoral', {'levels': [10, 5]}),
        ('box', 9, 3, 31, 43, 'solidnavy', {'levels': [5]})]},
    # Program: a page with OPL on it
    AP + 'TextEd/TextEd.aif#48x48': {'regions': [
        ('seed', 10, 11, 'logocoral', {'box': (4, 9, 17, 24)}),
        ('seed', 24, 18, 'logoleaf', {'box': (14, 17, 28, 34)}),
        ('seed', 33, 25, 'logosky', {'box': (26, 23, 42, 34)}),
        ('box', 0, 0, 47, 47, 'text', {'levels': [5, 7.5, 10]})]},
    # Time: a globe and a clock
    AP + 'TimeW/TimeW.aif#48x48': {'regions': [
        ('box', 0, 24, 21, 46, 'solidcoral', {'levels': [5, 2.5]}),
        ('box', 2, 26, 20, 44, 'clockface', {'levels': [12.5, 10, 15, 7.5]}),
        ('box', 14, 40, 40, 47, 'solidnavy', {'levels': [5, 2.5]}),
        ('box', 39, 3, 47, 40, 'solidcoral', {'levels': [15]}),
        ('box', 7, 3, 44, 40, 'land', {'levels': [5, 2.5]}),
        ('box', 7, 3, 44, 40, 'ocean', {'levels': [15, 12.5, 10, 7.5]})]},
})

MATERIALS.update({
    'board': {'base': '#f5b942', 'levels': {15: 1, 12.5: 1, 10: 2, 7.5: 3, 5: 4, 2.5: 5, 0: 6}},
    'tealdark': {'base': '#0f766e', 'levels': {15: 1, 12.5: 2, 10: 2, 7.5: 3, 5: 3, 2.5: 4, 0: 5}},
    'lcd': {'ramp': ['#eefaf7', '#d9f3ee', '#bfe9e1', '#9fdcd2', '#7dd3c8', '#4fb3a7', '#2f7f77'],
            'levels': {15: 1, 12.5: 2, 10: 3, 7.5: 4, 5: 5, 2.5: 6, 0: 6}},
})

ICONS.update({
    # Bombs: a warning sign with a skull
    AP + 'Bombs/Bombs.aif#48x48': {'regions': [
        ('box', 17, 32, 22, 46, 'metal', {'levels': [5, 15, 10]}),
        ('box', 3, 0, 40, 36, 'board', {'levels': [15, 12.5, 10, 7.5, 5]})]},
    # Record: a cassette recorder
    AP + 'Record/Record.aif#48x48': {'regions': [
        ('box', 13, 6, 46, 25, 'solidteal', {'levels': [5]}),
        ('box', 0, 0, 47, 47, 'solidplum', {'levels': [15, 12.5, 10, 7.5, 5, 2.5]})]},
    # Calc: a calculator
    AP + 'Calc/Calc.aif#48x48': {'regions': [
        ('seed', 14, 10, 'lcd', {'box': (5, 4, 26, 17)}),
        ('box', 0, 11, 12, 41, 'tealdark', {'levels': [5, 2.5]}),
        ('box', 12, 12, 40, 36, 'solidnavy', {'levels': [5]}),
        ('box', 0, 0, 47, 47, 'solidteal', {'levels': [10, 15, 12.5, 7.5]})]},
    # Install: a computer with the Psion hand, a disk, a CD
    'System/Apps/InstApp/InstApp.mbm#0': {'regions': [
        ('box', 10, 34, 30, 47, 'solidnavy', {'levels': [2.5, 5, 10, 7.5]}),
        ('box', 34, 16, 47, 26, 'solidteal', {'levels': [10, 5]}),
        ('box', 28, 28, 47, 40, 'solidplum', {'levels': [5]}),
        ('box', 28, 28, 47, 40, 'solidsky', {'levels': [10]}),
        ('box', 14, 4, 32, 33, 'clockface', {'levels': [12.5, 10]}),
        ('box', 0, 9, 14, 34, 'solidsky', {'levels': [10, 5]})]},
})

MATERIALS.update({
    'textink': {'base': '#2f6fae', 'levels': {0: 3, 2.5: 3, 5: 3, 7.5: 2, 10: 1, 12.5: 0, 15: 0}},
})

DM = 'System/Apps/Data/Data.mbm#'
ICONS.update({
    # Contacts: a card file on a stand
    AP + 'Contacts/Contacts.aif#48x48': {'regions': [
        ('box', 30, 6, 40, 18, 'solidcoral', {'levels': [5, 10, 15]}),
        ('box', 0, 0, 47, 47, 'solidnavy', {'levels': [5, 7.5, 2.5]}),
        ('box', 8, 0, 47, 30, 'text', {'levels': [10]})]},
    # Data's toolbar
    DM + '3': {'regions': [('box', 0, 0, 22, 18, 'solidteal', {'levels': [5]}),
                           ('box', 6, 4, 21, 20, 'textink', {'ink': True, 'levels': [0]})]},
    DM + '5': {'regions': [('box', 0, 0, 23, 23, 'solidteal', {'levels': [5]})]},
    DM + '7': {'regions': [('box', 5, 3, 17, 14, 'logocoral', {'levels': [5]}),
                           ('box', 0, 6, 10, 23, 'solidteal', {'levels': [5]}),
                           ('box', 11, 8, 23, 21, 'solidsky', {'levels': [7.5, 10, 12.5]})]},
    DM + '9': {'regions': [('box', 0, 0, 16, 20, 'solidteal', {'levels': [5]}),
                           ('box', 4, 5, 20, 13, 'card', {'levels': [10, 12.5]}),
                           
                           ('box', 8, 14, 22, 22, 'textink', {'levels': [5]})]},
    # small pictures of the built-in programs (Insert object and the like)
    EK + '24': {'regions': [('box', 14, 8, 23, 22, 'solidleaf', {'levels': [10]}),
                            ('box', 0, 0, 13, 23, 'solidnavy', {'levels': [10]})]},
    EK + '26': {'regions': [('box', 4, 5, 7, 22, 'solidcoral', {'levels': [5, 15]}),
                            ('box', 8, 3, 16, 22, 'solidnavy', {'levels': [5, 7.5, 10]})]},
    EK + '28': {'regions': [('box', 15, 1, 19, 13, 'solidsky', {'levels': [5, 10]}),
                            ('box', 11, 5, 15, 16, 'solidleaf', {'levels': [5, 10]}),
                            ('box', 7, 8, 11, 17, 'solidamber', {'levels': [5, 10]}),
                            ('box', 3, 11, 7, 18, 'solidcoral', {'levels': [5, 10]})]},
    EK + '30': {'regions': [('box', 0, 0, 23, 23, 'card', {'levels': [10, 7.5]}),
                            ('box', 0, 0, 23, 23, 'solidnavy', {'levels': [5]})]},
})

ICONS.update({
    # OPL: the letters, in the colours of Program's
    AP + 'OPL/OPL.aif#48x48': {'regions': [
        ('box', 0, 4, 15, 34, 'logocoral', {'ink': True, 'levels': [0, 5, 7.5, 10, 15]}),
        ('box', 16, 1, 27, 31, 'logoleaf', {'ink': True, 'levels': [0, 5, 7.5, 15]}),
        ('box', 28, 1, 47, 31, 'logosky', {'ink': True, 'levels': [0, 5, 7.5, 15]}),
        ('box', 0, 0, 47, 47, 'none', {'levels': [10]})]},
})

# Where the 5mx sides of pictures that are not in s7icons.json come from
# (tools/modicons.py --sides): installed name -> built file (from the top of
# the repository) or a file in the 5mx ROM ('rom:...', with --rom)
SOURCES = {
    'System/Apps/PsiTerm/PsiTerm.aif': 'pkg/psiterm.aif',
    'System/Apps/PsiTerm/PsiTerm.mbm': 'build/icons/psiterm.mbm',
    'System/Apps/PsiMail/PsiMail.aif': 'build/mail-pkg/psimail.aif',
    'System/Apps/PsiMail/PsiMail.mbm': 'build/mail-pkg/psimail.mbm',
    'System/Apps/PsiWeb/PsiWeb.aif': 'build/web-pkg/psiweb.aif',
    'System/Apps/PsiWeb/PsiWeb.mbm': 'build/web-pkg/psiweb.mbm',
    'System/Apps/PsiCast/PsiCast.aif': 'build/cast-pkg/psicast.aif',
    'System/Apps/PsiCast/PsiCast.mbm': 'build/cast-pkg/psicast.mbm',
    'System/Apps/Comms/Comms.aif': 'rom:Z:\\System\\Apps\\Comms\\Comms.aif',
    'System/Apps/MsgApp/MsgApp.aif': 'rom:Z:\\System\\Apps\\MsgApp\\MsgApp.aif',
}


# ---------------------------------------------------------------- our programs
# Each has an accent, kept across its icon's sizes and its toolbar:
# PsiTerm teal, PsiMail coral, PsiWeb sky, PsiCast plum.

def B(x0, y0, x1, y1, mat, levels=None, ink=False, force=False):
    """a box region (see the header)"""
    o = {}
    if levels is not None: o['levels'] = list(levels)
    if ink: o['ink'] = True
    if force: o['force'] = True
    return ('box', x0, y0, x1, y1, mat, o)

ALL = (0, 2.5, 5, 7.5, 10, 12.5, 15)
for _acc, _base in (('teal', '#14b8a6'), ('coral', '#f97362'), ('sky', '#4aa3df'), ('plum', '#8b6cc8'),
                    ('amber', '#f5b942'), ('leaf', '#5bbf6a'), ('navy', '#1f3a5f')):
    # fill: white is a mid tint, light grey the colour, dark grey darker
    MATERIALS['fill' + _acc] = {'base': _base, 'levels': {15: 2, 12.5: 2, 10: 3, 7.5: 4, 5: 4, 2.5: 5, 0: 6}}
    # pale: white the palest tint, light grey a tint, dark grey the colour
    MATERIALS['pale' + _acc] = {'base': _base, 'levels': {15: 0, 12.5: 1, 10: 1, 7.5: 2, 5: 3, 2.5: 4, 0: 5}}
    # logo: black is the colour (a black shape coloured in)
    MATERIALS['logo' + _acc] = {'base': _base, 'levels': {0: 3, 2.5: 4, 5: 4, 7.5: 2, 10: 2, 12.5: 1, 15: 1}}
    # deep: black is the colour's darkest step (screens, solid bodies)
    MATERIALS['deep' + _acc] = {'base': _base, 'levels': {0: 5, 2.5: 5, 5: 4, 7.5: 3, 10: 2, 12.5: 1, 15: 0}}
MATERIALS.update({
    'slate': {'ramp': ['#2b3445'] * 7},
    'screen': {'ramp': ['#7dd3c8', '#4a6a94', '#33507a', '#284568', '#1f3a5f', '#1a3150', '#152842']},
})

PT = 'System/Apps/PsiTerm/PsiTerm.'
PM = 'System/Apps/PsiMail/PsiMail.'
PW = 'System/Apps/PsiWeb/PsiWeb.'
PC = 'System/Apps/PsiCast/PsiCast.'

ICONS.update({
    # PsiTerm: a terminal window (teal title bar, its three buttons, a dark
    # screen with a teal prompt) and an amber padlock
    PT + 'aif#48x48': {'outline': True, 'regions': [
        B(6, 7, 7, 9, 'fillcoral', ALL), B(10, 7, 11, 9, 'fillamber', ALL), B(14, 7, 15, 9, 'fillleaf', ALL),
        B(3, 6, 42, 11, 'paleteal', (10,)),
        B(33, 37, 47, 47, 'fillamber', (10,)), B(31, 27, 47, 47, 'slate', (0,), ink=True),
        B(3, 13, 30, 30, 'paleteal', (15,)), B(3, 13, 30, 30, 'fillteal', (10,)),
        B(3, 12, 42, 40, 'screen', (0,), ink=True)]},
    PT + 'aif#32x32': {'outline': True, 'regions': [
        B(5, 4, 5, 6, 'fillcoral', ALL), B(8, 4, 8, 6, 'fillamber', ALL), B(11, 4, 11, 6, 'fillleaf', ALL),
        B(2, 3, 28, 7, 'paleteal', (10,)),
        B(21, 25, 31, 31, 'fillamber', (10,)), B(19, 18, 31, 31, 'slate', (0,), ink=True),
        B(3, 9, 20, 20, 'paleteal', (15,)), B(3, 9, 20, 20, 'fillteal', (10,)),
        B(2, 8, 30, 26, 'screen', (0,), ink=True)]},
    PT + 'aif#24x24': {'outline': True, 'regions': [
        B(2, 2, 22, 5, 'paleteal', (10,)),
        B(14, 17, 23, 23, 'fillamber', (10,)), B(13, 11, 23, 23, 'slate', (0,), ink=True),
        B(3, 7, 12, 15, 'paleteal', (15,)), B(3, 7, 12, 15, 'fillteal', (10,)),
        B(2, 6, 22, 16, 'screen', (0,), ink=True)]},
    # the toolbar: SSH to, Disconnect, Snippets, Keys, Files
    PT + 'mbm#0': {'regions': [B(3, 2, 21, 11, 'logoteal', (0,), ink=True), B(2, 13, 21, 13, 'fillteal', (5,)),
                               B(7, 18, 16, 18, 'fillteal', (5,))]},
    PT + 'mbm#2': {'regions': [B(9, 3, 16, 17, 'pairecoral' if False else 'palecoral', (15,)),
                               B(17, 4, 23, 16, 'paleteal', (15,)), B(0, 9, 8, 11, 'palecoral', (15,))]},
    PT + 'mbm#4': {'regions': [B(6, 5, 18, 15, 'logoteal', (0,), ink=True)]},
    PT + 'mbm#6': {'regions': [B(4, 3, 13, 7, 'logoteal', (0,), ink=True), B(2, 11, 20, 18, 'fillteal', (5,)),
                               B(11, 8, 22, 17, 'paleteal', (15,))]},
    PT + 'mbm#8': {'regions': [B(0, 4, 16, 18, 'folder', (15, 5)), B(17, 0, 23, 19, 'logoteal', (0,), ink=True)]},
    # the file browser's small pictures: folder, open folder, file, up, the server
    PT + 'mbm#10': {'regions': [B(0, 0, 15, 13, 'folder', (10,))]},
    PT + 'mbm#12': {'regions': [B(0, 0, 15, 13, 'folder', (15, 10))]},
    PT + 'mbm#14': {'regions': [B(4, 4, 11, 10, 'text', (5,))]},
    PT + 'mbm#16': {'regions': [B(0, 0, 15, 13, 'logoteal', (0,), ink=True)]},
    PT + 'mbm#18': {'regions': [B(2, 2, 13, 6, 'screen', (15,)), B(2, 2, 13, 6, 'logoteal', (0,), ink=True),
                                B(4, 8, 11, 9, 'fillteal', (0,), ink=True)]},
    # the file view's toolbar: Up, Favourite, Get, Send here, Close; a small star
    PT + 'mbm#20': {'regions': [B(0, 6, 19, 18, 'folder', (15, 5)), B(5, 0, 18, 10, 'logoteal', (0,), ink=True)]},
    PT + 'mbm#22': {'outline': 'all', 'regions': [B(0, 0, 11, 19, 'fillamber', (15,))]},
    PT + 'mbm#24': {'regions': [B(5, 0, 18, 12, 'fillteal', (15, 7.5)), B(2, 13, 21, 19, 'paleteal', (15, 5))]},
    PT + 'mbm#26': {'regions': [B(5, 0, 18, 12, 'fillteal', (15, 7.5)), B(2, 13, 21, 19, 'paleteal', (15, 5))]},
    PT + 'mbm#28': {'regions': [B(0, 6, 7, 14, 'fillteal', (15, 7.5)), B(10, 2, 23, 11, 'logoteal', (0,), ink=True),
                                B(9, 13, 23, 18, 'fillteal', (5,))]},
    PT + 'mbm#30': {'outline': 'all', 'regions': [B(0, 0, 15, 13, 'fillamber', (15, 7.5))]},

    # PsiMail: a coral envelope, the letter's flap drawn on it
    PM + 'aif#48x48': {'outline': True, 'regions': [B(8, 15, 39, 32, 'palecoral', (15, 12.5, 10, 5)),
                                                    B(0, 0, 47, 47, 'logocoral', ALL, ink=True)]},
    PM + 'aif#32x32': {'outline': True, 'regions': [B(5, 9, 26, 22, 'palecoral', (15, 12.5, 10, 5)),
                                                    B(0, 0, 31, 31, 'logocoral', ALL, ink=True)]},
    PM + 'aif#24x24': {'outline': True, 'regions': [B(4, 7, 19, 16, 'palecoral', (15, 12.5, 10, 5)),
                                                    B(0, 0, 23, 23, 'logocoral', ALL, ink=True)]},
    # the folder tree: Folder, Open folder, Inbox, Outbox, Drafts, Sent,
    # Deleted, Archive, Junk, Calendar
    PM + 'mbm#0': {'regions': [B(0, 0, 15, 12, 'folder', (15, 10, 5))]},
    PM + 'mbm#2': {'regions': [B(0, 0, 15, 12, 'folder', (15, 10, 5))]},
    PM + 'mbm#4': {'regions': [B(5, 0, 11, 5, 'fillcoral', (15,)), B(0, 6, 15, 12, 'palesky', (15, 5))]},
    PM + 'mbm#6': {'regions': [B(5, 0, 11, 5, 'fillcoral', (15,)), B(0, 6, 15, 12, 'palesky', (15, 5))]},
    PM + 'mbm#8': {'regions': [B(7, 2, 14, 10, 'fillamber', (5,)), B(2, 1, 12, 11, 'text', (10,))]},
    PM + 'mbm#10': {'regions': [B(0, 0, 15, 12, 'palesky', (10, 5))]},
    PM + 'mbm#12': {'regions': [B(0, 0, 15, 12, 'metal', (10, 5, 15))]},
    PM + 'mbm#14': {'regions': [B(1, 1, 14, 3, 'fillamber', (10, 5)), B(2, 5, 13, 11, 'paleamber', (15, 5)),
                                B(6, 6, 9, 8, 'fillamber', (5,))]},
    PM + 'mbm#16': {'regions': [B(3, 3, 12, 9, 'logocoral', (0,), ink=True), B(0, 0, 15, 12, 'palecoral', (15,))]},
    PM + 'mbm#18': {'regions': [B(0, 0, 15, 3, 'logocoral', (0, 15), ink=True), B(1, 4, 14, 11, 'metal', (5,))]},
    # a message's state: unread, read, replied, flagged, attachment, draft, error
    PM + 'mbm#20': {'regions': [B(0, 0, 13, 10, 'paleamber', (15,))]},
    PM + 'mbm#22': {'regions': [B(0, 0, 13, 4, 'paper', (15,)), B(0, 0, 13, 10, 'fillamber', (10,)),
                                B(0, 4, 13, 10, 'paleamber', (15,))]},
    PM + 'mbm#24': {'regions': [B(3, 4, 11, 9, 'logocoral', (0,), ink=True), B(0, 0, 13, 4, 'paper', (15,)),
                                B(0, 0, 13, 10, 'fillamber', (10,)), B(0, 4, 13, 10, 'paleamber', (15,))]},
    PM + 'mbm#26': {'regions': [B(0, 0, 13, 10, 'fillcoral', (5,))]},
    PM + 'mbm#28': {'outline': 'all', 'regions': [B(0, 0, 13, 10, 'fillsky', (15,))]},
    PM + 'mbm#30': {'regions': [B(7, 5, 11, 9, 'fillamber', (7.5, 5)), B(0, 0, 13, 10, 'text', (10,))]},
    PM + 'mbm#32': {'regions': [B(0, 0, 13, 10, 'fillamber', (15,))]},
    # the calendar's rows: alarm, repeat, pending; and Outbox's envelope
    PM + 'mbm#34': {'regions': [B(0, 0, 10, 8, 'fillamber', (15,))]},
    PM + 'mbm#36': {'outline': 'all', 'regions': [B(0, 0, 10, 9, 'fillsky', (15,))]},
    PM + 'mbm#38': {'regions': [B(0, 0, 10, 8, 'fillcoral', (15,))]},
    PM + 'mbm#40': {'regions': [B(4, 0, 9, 4, 'fillcoral', (15,)), B(0, 5, 13, 10, 'paleamber', (15,))]},
    # the toolbar: New, Reply, Check, Delete, Back
    PM + 'mbm#42': {'regions': [B(13, 0, 23, 6, 'fillamber', (15, 5)), B(0, 6, 23, 19, 'palecoral', (15,))]},
    PM + 'mbm#44': {'outline': 'all', 'regions': [B(0, 0, 23, 19, 'fillcoral', (15,))]},
    PM + 'mbm#46': {'regions': [B(8, 0, 15, 7, 'fillcoral', (15,)), B(0, 8, 23, 18, 'palesky', (15,))]},
    PM + 'mbm#48': {'regions': [B(0, 0, 23, 19, 'metal', (10, 5, 15))]},
    PM + 'mbm#50': {'regions': [B(0, 0, 18, 9, 'fillcoral', (15,)), B(3, 11, 22, 19, 'palecoral', (15,))]},

    # PsiWeb: a sky-blue globe with its lines of latitude and longitude
    PW + 'aif#48x48': {'outline': True, 'regions': [B(0, 0, 47, 47, 'ocean', (10, 5)),
                                                    B(4, 6, 43, 42, 'navy', (0,), ink=True)]},
    PW + 'aif#32x32': {'outline': True, 'regions': [B(0, 0, 31, 31, 'ocean', (10, 5)),
                                                    B(3, 4, 28, 28, 'navy', (0,), ink=True)]},
    PW + 'aif#24x24': {'outline': True, 'regions': [B(0, 0, 23, 23, 'ocean', (10, 5)),
                                                    B(3, 3, 20, 20, 'navy', (0,), ink=True)]},
    # the toolbar: Open address (a globe), Back, Home page, Pictures
    PW + 'mbm#0': {'regions': [B(0, 0, 23, 19, 'fillsky', (15,))]},
    PW + 'mbm#2': {'outline': 'all', 'regions': [B(0, 0, 23, 19, 'fillsky', (15,))]},
    PW + 'mbm#4': {'regions': [B(0, 0, 23, 9, 'fillcoral', (15,)), B(4, 10, 19, 18, 'paleamber', (15,)),
                               B(6, 14, 13, 18, 'fillsky', (15,))]},
    PW + 'mbm#6': {'regions': [B(12, 3, 17, 7, 'fillamber', (5,)), B(1, 7, 18, 15, 'fillleaf', (5,)),
                               B(1, 15, 18, 17, 'fillleaf', (10,)), B(1, 1, 18, 15, 'palesky', (15,))]},

    # PsiCast: a plum screen with the waves of it going out
    PC + 'aif#48x48': { 'regions': [B(4, 10, 28, 24, 'fillplum', (5,)),
                                                    B(0, 0, 47, 47, 'logoplum', (0,), ink=True)]},
    PC + 'aif#32x32': { 'regions': [B(3, 6, 18, 16, 'fillplum', (5,)),
                                                    B(0, 0, 31, 31, 'logoplum', (0,), ink=True)]},
    PC + 'aif#24x24': { 'regions': [B(2, 4, 14, 12, 'fillplum', (5,)),
                                                    B(0, 0, 23, 23, 'logoplum', (0,), ink=True)]},
    # the toolbar: Share, Pause, Control, Previous
    PC + 'mbm#0': {'regions': [B(2, 4, 8, 12, 'fillplum', (5,)), B(9, 0, 19, 12, 'logoplum', (0,), ink=True)]},
    PC + 'mbm#2': {'regions': [B(0, 0, 19, 19, 'logoplum', (0,), ink=True)]},
    PC + 'mbm#4': {'regions': [B(0, 0, 19, 3, 'logoplum', (0,), ink=True), B(0, 4, 19, 19, 'paleamber', (15,))]},
    PC + 'mbm#6': {'regions': [B(0, 4, 9, 11, 'logoplum', (0,), ink=True), B(8, 2, 19, 3, 'fillplum', (5,)),
                               B(8, 4, 19, 13, 'paleplum', (15,))]},
})

ICONS.update({
    # Comms: a 5mx (its screen sky blue) and a navy mobile phone
    AP + 'Comms/Comms.aif#48x48': {'regions': [
        B(37, 9, 45, 15, 'lcd', (10,)), B(33, 0, 47, 35, 'solidnavy', (5, 2.5)),
        B(33, 0, 47, 35, 'palesky', (15,)),
        ('seed', 20, 8, 'palesky'),
        B(0, 0, 47, 47, 'metal', (10, 5, 2.5, 15, 12.5))]},
    # Email: a letter flying past the globe (green land, blue sea)
    AP + 'MsgApp/MsgApp.aif#48x48': {'outline': True, 'regions': [('seed', 14, 18, 'paper'),
        B(0, 0, 47, 47, 'land', (5, 2.5, 7.5)), B(0, 0, 47, 47, 'ocean', (15, 12.5, 10))]},
    AP + 'MsgApp/MsgApp.aif#32x32': {'outline': True, 'regions': [('seed', 8, 12, 'paper'),
        B(0, 0, 31, 31, 'land', (5, 2.5, 7.5)), B(0, 0, 31, 31, 'ocean', (15, 12.5, 10))]},
    AP + 'MsgApp/MsgApp.aif#24x24': {'outline': True, 'regions': [('seed', 7, 9, 'paper'),
        B(0, 0, 23, 23, 'land', (5, 2.5, 7.5)), B(0, 0, 23, 23, 'ocean', (15, 12.5, 10))]},
})
