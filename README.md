# LittleBigPlanet 3 — PC (port nativo, não oficial)

Port experimental de **LittleBigPlanet 3** (PS4) para **Windows 10/11 (64 bits)**, em andamento.
O executável original do jogo roda nativamente no processador (o PS4 é x86-64), as bibliotecas do
sistema são reimplementadas pelo projeto e a parte gráfica é traduzida para **Vulkan** com o
núcleo de vídeo do shadPS4. É um fork do [`gow3_pc`](https://github.com/FelipePrado0/gow3_pc), o mesmo
caminho do [Hotline Miami 2 — PC](https://github.com/TheNIKITO/Hotline-Miami-2---PC).

> **Aviso:** projeto de estudo, sem ligação com a Sony Interactive Entertainment, a Sumo Digital
> ou a Media Molecule. LittleBigPlanet é marca da Sony Interactive Entertainment.
> **Nenhum arquivo do jogo está incluído.** Você precisa da sua própria cópia, já extraída.

## Estado do projeto

<!-- status:begin -->
![decompilação: não se aplica](https://img.shields.io/badge/decompila%C3%A7%C3%A3o-n%C3%A3o%20se%20aplica-lightgrey) ![executável: 100%](https://img.shields.io/badge/execut%C3%A1vel-100%25-brightgreen) ![jogo: 50%](https://img.shields.io/badge/jogo-50%25-orange)

| Parte | Progresso | O que o número mede |
| --- | --- | --- |
| Decompilação | não se aplica | O executável original roda direto no processador (o PS4 é x86-64): nada precisa ser decompilado. |
| Executável | `██████████` **100%** | 636 de 636 funções do sistema que o `eboot.bin` e seus módulos chamam já têm resposta no port (serviços online, câmera e PS Move respondem como ausentes). |
| Jogo | `█████░░░░░` **50%** | 4 de 8 marcos da lista abaixo. |

Marcos do jogo:

- [x] O executável abre e roda nativamente
- [x] O motor inicializa (threads, memória, leitura dos .farc)
- [x] Módulos próprios do jogo carregam (spu.prx, plugins FMOD)
- [x] Primeiro frame na tela
- [ ] Menu principal
- [ ] Uma fase jogável
- [ ] Áudio (FMOD)
- [ ] Saves

_Números medidos em 2026-10-18 (`python tools/lbp3_status.py`)._
<!-- status:end -->

## Versão do jogo usada nos testes

Testado em Windows 11 com uma RTX 5060 Ti:

| | |
| --- | --- |
| Título | LittleBigPlanet 3 |
| ID do título | `CUSA00473` (EUA) |
| ID do conteúdo | `UP9000-CUSA00473_00-LBP3GAME00000001` |
| Versão do jogo (`APP_VER`) | `01.00` |
| Versão master (`VERSION`) | `01.02` |
| SDK do executável | 1.70 |
| SHA-256 do `eboot.bin` | `c67ea6a4ad18a63c0fe64d207c341f6cd5f2a0aa95c2b36c69f2879e4000cd70` |

Para conferir a sua cópia, rode no PowerShell, dentro da pasta do jogo:
`Get-FileHash .\eboot.bin -Algorithm SHA256`.

## Onde o jogo está agora

O jogo abre, carrega os módulos que vêm com ele (`gamedata_orbis/spu/spu.prx` e quatro plugins do
FMOD), reconhece o controle e **desenha a primeira tela**: o carregamento "Connecting to
LittleBigPlanet™3 Online…", com a arte e as fontes certas.

Ele não passa dessa tela. O gerenciador de rede do jogo tem 24 tarefas, e a tela fica enquanto
alguma estiver "em andamento"; sem rede e fora da PSN, uma delas (e a que depende dela) nunca
termina. Também há uma queda intermitente nos primeiros segundos, em mais ou menos metade das
aberturas.

Próximos passos:
1. Descobrir o que a tarefa de rede presa espera e responder como um PS4 offline responderia.
2. Achar a queda intermitente do começo.
3. Menu principal, áudio do FMOD e saves.

Ainda não há pacote pronto nem launcher próprio: por enquanto o jogo só roda a partir do código.

## Compilar e rodar a partir do código

1. Instale o [MSYS2](https://www.msys2.org) e, no shell **MSYS2 CLANG64**, os pacotes listados em
   [packaging/windows/README.md](packaging/windows/README.md). Instale também o
   [Python 3](https://www.python.org) com Pillow (`pip install pillow`).
2. Clone com os submódulos, **sem converter as quebras de linha** (o patch do FSR só aplica com LF):

   ```bash
   git clone --recursive -c core.autocrlf=false https://github.com/TheNIKITO/LittleBigPlanet-3---PC lbp3_pc
   cd lbp3_pc
   bash build.sh
   ```

3. Rode, no PowerShell, apontando para a pasta do **seu** jogo (a que tem `eboot.bin`,
   `sce_module`, `sce_sys` e os `.farc`):

   ```powershell
   $env:GOW3_GAME_DIR = 'C:\caminho\CUSA00473'
   $env:GOW3_DATA_DIR = "$PWD\lbp3-data"
   python run.py
   ```

   Saves, caches e o log ficam em `lbp3-data`.

Para listar as funções do sistema que o jogo chama e que o port ainda não responde, sem rodar o jogo:

```powershell
.\out\gow3-probe.exe lbp3-data\out\boot-linked.bin --app0 $env:GOW3_GAME_DIR --check-imports
```

## O que este fork muda

| Arquivo | Para quê |
| --- | --- |
| `scripts/link_modules.py` | Também linka os módulos próprios do jogo que o eboot declara como dependência (achados pelo nome do módulo), marcados para iniciar quando o jogo os carrega; formato G3PROBE6, com o nome de cada módulo |
| `src/runtime_module.c` | `sceKernelLoadStartModule`: acha o módulo já linkado pelo nome do arquivo e roda o início dele com os argumentos do jogo |
| `src/runtime_fiber.c` | Fibers (`libSceFiber`) sobre fibers do Windows (ucontext no Linux) |
| `src/runtime_evf.c` | Event flags do kernel (`sceKernel*EventFlag`), usados para sincronizar as threads do motor |
| `src/runtime_sema.c` | Semáforos POSIX (`sem_init`, `sem_wait`, `sem_timedwait`…) sobre os semáforos do kernel |
| `src/runtime_thread.c` | Prioridade de threads (`scePthreadGet/Setschedparam`) e `scePthreadAttrSetstack` |
| `src/runtime_content.c` | Dados temporários em `/temp0` e chave de DLC (nenhum DLC instalado) |
| `src/runtime_services.c` | Respostas "offline/ausente" para câmera, PS Move, transmissão ao vivo, app companheiro, HTTP, PSN, sockets POSIX, JPEG e diálogo de erro; cada uma avisa no log quando o jogo a usa. Os callbacks de rede (NetCtl) ouvem "desconectado" a cada verificação, como no console sem cabo |
| `src/import_names.inc` | Nomes das funções que o LittleBigPlanet 3 importa |
| `src/probe.c`, `run.py`, `gpu/shim/window.cpp` | Os hooks, as cópias de arquivos e a captura do mouse do God of War III só entram para o God of War III: nos outros jogos o cursor fica livre e visível |
| `src/probe.c` (diagnóstico) | `GOW3_IMPORT_STATS=1` conta as chamadas de cada função do sistema e mostra, a cada 5 s, as mais chamadas (com quem chamou) e as chamadas pela primeira vez; `GOW3_WATCH=offset:quantidade[:tamanho]` imprime memória do jogo junto |
| `tools/disasm.py` | Desmonta um trecho do jogo pelo offset que o loader mostra, com as funções do PS4 chamadas nomeadas (usa o `llvm-objdump` do CLANG64) |
| `scripts/link_libc.py` | Aceita o `libc.prx` do LittleBigPlanet 3 (outra versão, com um símbolo sem NID) |
| `tools/import_names.py` | Dá nome às importações de um jogo a partir da lista pública do shadPS4 (`aerolib.inl`) |
| `tools/lbp3_status.py`, `docs/lbp3/status.json` | O estado do projeto acima e no `.recomp.json` |
| `.recomp.json` | Ficha do projeto lida pelo [recomp.board](https://recomp.fyi) |

## Atualizar o estado do projeto

Os números do topo e as notas do `.recomp.json` saem de `docs/lbp3/status.json`. Marque os marcos à
mão nesse arquivo; as funções respondidas vêm de uma execução do `--check-imports`:

```powershell
.\out\gow3-probe.exe lbp3-data\out\boot-linked.bin --app0 $env:GOW3_GAME_DIR --check-imports > lbp3-data\check-imports.txt
python tools\lbp3_status.py --image lbp3-data\out\boot-linked.bin --check lbp3-data\check-imports.txt
```

Sem argumentos, o script só reescreve o README e o `.recomp.json` a partir do `status.json`.

## Créditos

- **Felipe Prado**: [gow3_pc](https://github.com/FelipePrado0/gow3_pc), a base deste projeto (runtime
  nativo, loader, launcher e a integração com o shadPS4).
- **Supermedo** (Mohammed Albarghouthi): [bloodborne_pc para Windows](https://github.com/Supermedo/bloodborne_pc),
  de onde o gow3_pc veio.
- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), o port nativo original.
- **cuesta4**: [fork do shadPS4](https://github.com/cuesta4/shadPS4/tree/eltutz) com as correções de
  renderização que o gow3_pc usa.
- **Equipe do shadPS4**: o núcleo de vídeo [shadPS4](https://github.com/shadps4-emu/shadPS4), que traduz
  a GPU e os shaders para Vulkan, e a lista pública de nomes de funções do PS4 (`aerolib.inl`).

Também usados: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), [MSYS2](https://www.msys2.org) e [LLVM](https://llvm.org).

## Licença

GNU GPL v2 ou posterior ([LICENSE](LICENSE)), a mesma do gow3_pc. Componentes de terceiros mantêm as
próprias licenças.
