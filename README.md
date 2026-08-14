# fabric-harness

The weft plane harness. Every plane and every edge links this, and none of them links
iceoryx2.

Split out of [`fabric-weft-plane`](https://github.com/v-sekai-multiplayer-fabric/fabric-weft-plane) with its
history. weft keeps only the data plane and the NIF the BEAM loads. A plane is its own
process, its own repository, and its own container.


Goal: one runtime model for every plane. A thin C++ thread-per-core loop over iceoryx2.

State: a first loop exists, compiled against the real ABI, not yet run.

- **Built.** `weft::harness`, a library every plane and edge links. It holds the bus, the
  limits, and the payload type.
- **Proved.** `proof/` passes a `Snapshot` between two processes with no copy and no
  daemon. The run is below.
- **Built, not proved.** `weft/loop.hpp`'s `run_command_loop` — a command in, reply bytes
  out, over a new payload variant (`iox2_type_variant_e_DYNAMIC`, a byte slice, not
  `Snapshot`'s fixed struct) and `weft/command.hpp`'s request-id-prefixed envelope.
  `proof/command_publisher.cpp`/`command_subscriber.cpp` exist and compile clean against
  the generated ABI header, but nobody has run them against a live `libiceoryx2_ffi_c` —
  this repo builds on any machine (see "Nothing links iceoryx2" below), the *proof* needs
  Linux and the real library, neither of which was available where this was written. Run
  the proof before anything links this loop into a real service.
- One thread, not thread-per-core: the goal above names the eventual shape, and a
  single-process, likely-GPU-bound interactor (one plane, this loop's first intended
  caller) has no per-core work to split. `run_command_loop`, not `run_loop`, so it doesn't
  claim to be that eventual answer.

## One harness, not one for each plane

weft has several planes and two edges, and the number grows. Each one needs the bus and
each one needs the limits.

Left alone, each would grow its own copy of both, and the copies would drift the way a
decision written twice always drifts. That is the failure `Weft.VocabularyTest` was
written for, in a different form.

So there is one. A plane repository brings this in with `git subtree add --prefix=thirdparty/harness`,
and links `weft::harness`.

| what it gives | where | why it is shared |
| --- | --- | --- |
| the bus | `iceoryx2.sigs`, and the table generated from it | one C ABI, one dispatch table |
| the limits | `include/weft/limits.hpp` | every value is `Weft.Limits`, which is rivet's |
| the payloads | `include/weft/snapshot.hpp`, `include/weft/store.hpp` | both ends of a service must agree exactly |

`Weft.PlaneNetworkingTest` holds that shape. It fails if a second `.sigs` file appears, if
a plane declares a limit of its own, or if a directory with a `CMakeLists.txt` is missing
from the root build.

## Nothing links iceoryx2

`iceoryx2.sigs` lists the 40 C ABI functions the harness calls. Chromium's
`generate_stubs.py`, vendored at `thirdparty/generate_stubs`, turns that list
into a dlsym dispatch table. The pattern comes from `fabric-godot-core`, which uses it for
GStreamer.

So the harness builds on a machine that has never seen iceoryx2, and it fails at start
rather than at link when the library is absent. `ldd` on either binary lists no iceoryx2.

That matters more here than it did for GStreamer. iceoryx2 is Rust, and weft writes no
Rust. A dlopen keeps the Rust artifact out of weft's build graph as well as out of its
source.

Three generated pieces come from that one file, and each has a reason.

- `iceoryx2_stubs.cc`, the dispatch table.
- `iox2_api.h`, the prototypes. `generate_stubs.py` emits none, because Chromium's callers
  include the real library headers. A prototype that disagrees with the table it calls
  through is a crash with no diagnostic, so both come from the same file.
- `src/iox2_decls.h` is hand-written, and it is the one piece that is not generated. It
  declares the opaque types. A handle is a pointer, so an incomplete struct is exact. A
  storage struct, `iox2_..._t`, is a real sized struct, and transcribing it would be a
  silent memory bug the day upstream adds a field. It stays incomplete, and the harness
  passes NULL for every one. iceoryx2 then allocates on the heap, which is the documented
  contract.

## What the proof is

Two programs and one struct.

- `src/snapshot.hpp` holds `weft::Snapshot`: a tick, an entity, and three positions in
  micrometres. This is the fixed point the data plane already uses.
- `src/publisher.cpp` loans a sample, writes the struct, and sends the loan. It does not
  serialize and it does not copy. `loan_uninit` returns memory the subscriber already
  maps, so a send is a pointer handoff.
- `src/subscriber.cpp` receives and checks. It checks the tick order and the payload it
  derives from the tick.

The check is the point. A bus that delivers garbage must fail here, and not print a count.

## How to run it

The build needs no iceoryx2. The run does. Build and install iceoryx2 v0.9.3 somewhere the
loader can find at run time. The prefix below matches the one `.gitignore` already
excludes.

    cmake -S <iceoryx2-src> -B <iceoryx2-src>/build \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/.iceoryx
    cmake --build <iceoryx2-src>/build -j
    cmake --install <iceoryx2-src>/build

Then build the proof and run both ends. Start the subscriber first.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

    export LD_LIBRARY_PATH=$PWD/.iceoryx/lib64   # or set WEFT_ICEORYX2_PATH
    ./build/weft-harness-subscriber 8 &
    ./build/weft-harness-publisher 8

## The run

One machine, 16 cores, Fedora, GCC 16.1.1, iceoryx2 v0.9.3, Release.

    publisher: sent 8
    subscriber: tick 1 entity 42 x 1000 z -250
    ...
    subscriber: tick 8 entity 42 x 8000 z -2000
    subscriber: received 8, in order, intact

The subscriber exits 0. No daemon runs, and none is started.

## The command/reply proof (`run_command_loop`) -- not yet run

Same setup as above, same expected shape: `weft-harness-command_subscriber` runs
`weft::run_command_loop` itself (server, echo `ask`), `weft-harness-command_publisher`
sends "ping N" and checks "pong N" comes back on the reply topic, correlated by request id.

    ./build/weft-harness-command_subscriber 8 &
    ./build/weft-harness-command_publisher 8

Expected exit: both 0, `command_publisher` printing `sent and confirmed 8`,
`command_subscriber` printing `answered 8, in order`. **This has not been run against a
real `libiceoryx2_ffi_c`** -- both files compile clean against the generated ABI header
(verified), which is as far as a machine with no Linux iceoryx2 install can check. If you
run this and it works, replace this paragraph with a real run log, the same way the
`Snapshot` proof above has one. If it doesn't, that's exactly what this section is for.

`ldd build/weft-harness-publisher` lists no iceoryx2. The library arrives
through `dlopen` at start.

## What this run does not measure

The latency and the rate. Eight messages at a 20 ms cycle measures that a message
arrives, and nothing else. A number belongs in `../logbook/data_plane.md` with the machine
and the settings that produced it, and this run produces none.

## Why iceoryx2 and not iceoryx v1

iceoryx v2.0.8, which is the C++ project, does not build here. Its Linux platform layer
includes `<sys/acl.h>` and links `acl`, and libacl is not allowed.

Neither part can be turned off from the command line. `LINUX` is a normal CMake variable
that shadows the cache, so `-DLINUX=OFF` does nothing, and `ICEORYX_PLATFORM` is a
`CACHE PATH FORCE`. Upstream ships an ACL-free `unix` layer, and reaching it needs a patch
to two build files.

weft's `docs/essays/runtime-choice.md` holds the full reversal, and the cost of it.

## What comes next

1. The thread-per-core loop, once, because every plane uses it.
2. iceoryx2 in the container image, so CI runs this proof rather than a person.
3. The first plane behind it. `zone-server-h2o` is the candidate, because its zone tick
   has no input at all until the bus carries one.

## Waiting

`iox2_node_wait` is a periodic sleep. It cannot wake on a packet, so a process holding both a
bus and a socket would need a second event loop for the network, and two event loops in one
process is the thing an edge should not have.

So the harness binds the **WaitSet**. It takes an arbitrary file descriptor through
`iox2_file_descriptor_new`, which accepts a raw `int`, and `iox2_waitset_attach_notification`
returns a guard that identifies which attachment fired. A UDP socket, a `timerfd` and the bus
therefore wait in one place.

The signatures are transcribed from `iceoryx2-ffi/c/src/api` at v0.9.3, which is the C ABI
itself rather than a generated header, because iceoryx2 generates its header at build time and
this repository must build without it.
