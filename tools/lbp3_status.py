#!/usr/bin/env python3
"""Progress of the LittleBigPlanet 3 port, written into README.md and .recomp.json.

docs/lbp3/status.json holds the numbers: how many of the system functions the executable and
its modules import the port answers, and the game's milestones (edited by hand there). The
import numbers come from a run of the loader:

    gow3-probe out/lbp3/boot-linked.bin --app0 <game> --check-imports > out/lbp3/check-imports.txt
    python tools/lbp3_status.py --image out/lbp3/boot-linked.bin --check out/lbp3/check-imports.txt

Without arguments it only rewrites the status block of README.md (between the status markers)
and the notes of .recomp.json from status.json.
"""
import argparse
import datetime
import json
import re
import sys
from pathlib import Path
from urllib.parse import quote

ROOT = Path(__file__).resolve().parent.parent
STATUS = ROOT / 'docs' / 'lbp3' / 'status.json'
README = ROOT / 'README.md'
RECOMP = ROOT / '.recomp.json'
BEGIN, END = '<!-- status:begin -->', '<!-- status:end -->'
MISSING = re.compile(r'^MISSING: [^#]+#(\S+) \((.*)\)$')
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_names import linked_imports  # noqa: E402


def percent(done, total):
    """Rounded down: the page never claims more than was done."""
    return done * 100 // total if total else 0


def bar(value):
    return '█' * (value // 10) + '░' * (10 - value // 10)


def badge(label, message, colour):
    def part(text):
        return quote(text.replace('-', '--').replace('_', '__'), safe='')
    return f'![{label}: {message}](https://img.shields.io/badge/{part(label)}-{part(message)}-{colour})'


def colour(value):
    return 'red' if value < 34 else 'orange' if value < 67 else 'yellowgreen' if value < 90 else 'brightgreen'


def measure(status, image, check):
    missing = []
    for line in Path(check).read_text(encoding='utf-8', errors='replace').splitlines():
        if m := MISSING.match(line):
            library, name = m.groups()
            # An export of the game's own modules has no public name: the module names it.
            missing.append(f'{library}: {name}' if not name.startswith('name not resolved')
                           else f'{library} (módulo do próprio jogo)')
    total = len(linked_imports(image))
    status['imports'] = dict(total=total, answered=total - len(missing), missing=missing)
    status['measured'] = datetime.date.today().isoformat()


def block(status):
    imports, milestones = status['imports'], status['milestones']
    executable = percent(imports['answered'], imports['total'])
    done = sum(m['done'] for m in milestones)
    game = percent(done, len(milestones))
    lines = [
        ' '.join((badge('decompilação', 'não se aplica', 'lightgrey'),
                  badge('executável', f'{executable}%', colour(executable)),
                  badge('jogo', f'{game}%', colour(game)))),
        '',
        '| Parte | Progresso | O que o número mede |',
        '| --- | --- | --- |',
        '| Decompilação | não se aplica | O executável original roda direto no processador (o PS4 é x86-64): '
        'nada precisa ser decompilado. |',
        f'| Executável | `{bar(executable)}` **{executable}%** | {imports["answered"]} de {imports["total"]} funções '
        'do sistema que o `eboot.bin` e seus módulos chamam já têm resposta no port (serviços online, câmera '
        'e PS Move respondem como ausentes). |',
        f'| Jogo | `{bar(game)}` **{game}%** | {done} de {len(milestones)} marcos da lista abaixo. |',
        '',
        'Marcos do jogo:',
        '',
        *[f'- [{"x" if m["done"] else " "}] {m["pt"]}' for m in milestones],
    ]
    if imports['missing']:
        lines += ['', 'Funções que o jogo chama e o port ainda não responde:', '',
                  *[f'- `{name}`' for name in imports['missing']]]
    lines += ['', f'_Números medidos em {status["measured"]} (`python tools/lbp3_status.py`)._']
    return '\n'.join(lines), executable, game, done


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, help='out/lbp3/boot-linked.bin of the measured run')
    parser.add_argument('--check', type=Path, help='output of gow3-probe --check-imports')
    args = parser.parse_args()
    if bool(args.image) != bool(args.check):
        parser.error('--image and --check go together')
    status = json.loads(STATUS.read_text(encoding='utf-8'))
    if args.image:
        measure(status, args.image, args.check)
        STATUS.write_text(json.dumps(status, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    text, executable, game, done = block(status)
    readme = README.read_text(encoding='utf-8')
    start, end = readme.index(BEGIN) + len(BEGIN), readme.index(END)
    README.write_text(readme[:start] + '\n' + text + '\n' + readme[end:], encoding='utf-8', newline='\n')
    recomp = json.loads(RECOMP.read_text(encoding='utf-8'))
    reached = [m['en'] for m in status['milestones'] if m['done']]
    imports = status['imports']
    recomp['notes'] = (f'Executable: {executable}% ({imports["answered"]} of {imports["total"]} imported PS4 system '
                       f'functions answered by the port). Game: {game}% ({done} of {len(status["milestones"])} '
                       f'milestones{": " + ", ".join(reached) if reached else ""}). {status["next_en"]}')
    recomp['x-progress'] = {'decompilation': 'not applicable', 'executable': executable, 'game': game}
    RECOMP.write_text(json.dumps(recomp, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f'Executável {executable}%, jogo {game}% ({done}/{len(status["milestones"])} marcos)')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        sys.exit(f'lbp3_status failed: {error}')
