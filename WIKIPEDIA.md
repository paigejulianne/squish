{{Infobox file format
| name                  = SQUISH
| icon                  =
| logo                  =
| screenshot            =
| extension             = <code>.sqsh</code>
| mimetype              =
| magic                 = <code>SQSH</code> followed by <code>0x0D 0x0A 0x1A 0x0A</code>
| developer             = Paige Julianne Sullivan
| latest release version = 1 (on-disk format)
| genre                 = [[Data compression]]
| containerfor          = Files and directory trees
| type                  = [[Lossless compression|Lossless]] [[data compression]]
| standard              = <code>docs/FORMAT.md</code> (self-published)
| free                  = Yes
| license               = [[GNU General Public License|GPLv3]]
}}

'''SQUISH''' is a [[lossless compression|lossless]] [[data compression]] format and its
reference implementation, a [[context mixing]] file compressor written in [[C (programming language)|C]].
It is distributed as a linkable library (<code>libsquish.so</code> / <code>squish.dll</code>)
together with a [[command-line interface|command-line]] tool. SQUISH trades speed for
[[compression ratio]]: on a 315&nbsp;MB combined standard-corpus benchmark it produces
smaller output than <code>zip&nbsp;-9</code>, <code>bzip2&nbsp;-9</code>, and <code>rar&nbsp;-m5</code>
on 23 of 24 files, and beats <code>xz&nbsp;-9e</code> on 19 of 24, at a cost of roughly
0.6&nbsp;MB/s throughput and about 150&nbsp;MB of model memory per active block.<ref name="readme">SQUISH project README.</ref>

The design rests on a single premise: '''compression is prediction'''. If the probability
of the next bit can be estimated accurately, an [[arithmetic coding|arithmetic coder]]
converts those probabilities into a near-optimal code, a consequence of [[Shannon's source coding theorem]].
Accordingly, essentially all of SQUISH's engineering is concentrated in an
[[online machine learning|online-learning]] predictor that adapts to whatever kind of data it reads.<ref name="squish-md">SQUISH.md — algorithm design document.</ref>

== Overview ==

Conventional general-purpose compressors each commit to one structural model of data:
[[zip (file format)|zip]] uses [[DEFLATE]] ([[LZ77 and LZ78|LZ77]] plus [[Huffman coding]]),
[[bzip2]] uses the [[Burrows–Wheeler transform]], and [[RAR (file format)|RAR]] combines
[[LZ77 and LZ78|LZ]] with prediction-by-partial-matching filters. SQUISH instead runs '''ten
specialist statistical models simultaneously''' and learns, bit by bit, which of them to
trust for the data currently being read. As a result it behaves like an LZ compressor on
repetitive data, like a high-order [[Prediction by partial matching|PPM]] compressor on
text, and like a table or raster codec on record-structured data, without being told in
advance which kind of file it is handling.<ref name="squish-md" />

Because the decompressor runs the identical predictor in lockstep — making the same
predictions and applying the same updates as the encoder — the format needs no stored
[[dictionary coder|dictionaries]], no code tables, and no block structure describing the
model. In the project's phrasing, "the model is the format": the constants and update rules
in the source file <code>squish.c</code> ''are'' the bitstream specification, and any change
to them constitutes a format break.<ref name="squish-md" /><ref name="format">docs/FORMAT.md — byte-level format specification.</ref>

Compression is [[symmetric-key algorithm|symmetric]] in cost: decompression runs the same
models as compression and therefore takes approximately the same time, distinguishing SQUISH
from asymmetric codecs such as xz, where decompression is far cheaper than compression.<ref name="readme" />

== Compression algorithm ==

SQUISH codes one bit at a time. For each bit, every model emits a probability that the bit
is 1; an online-trained mixer fuses those opinions into a single probability, two refinement
stages recalibrate it, and an arithmetic coder emits the corresponding fraction of a bit. The
pipeline runs in the order ''predict, code, update'' for both encoder and decoder.<ref name="format" />

=== Adaptive counters ===

Most contexts are represented by a 32-bit ''adaptive counter'' holding a 16-bit probability
and a 16-bit confidence count, initialized to probability 0.5 with zero confidence. After
each bit the probability moves toward the observed outcome by a step of {{nowrap|1/(''n''&nbsp;+&nbsp;2)}},
where ''n'' is the confidence count, which is then incremented (capped at 255). The counter
is therefore highly plastic when a context is fresh and stabilizes as evidence accumulates,
giving fast convergence on new contexts and stability on well-established ones.<ref name="format" />

=== The ten models ===

Eight of the models are hashed context models sharing a table of 2<sup>22</sup> counters, each
indexed by a 64-bit [[hash function]] of its context combined with the partial current byte:<ref name="format" />

* '''Order-1 through order-6 byte contexts''' (orders 1, 2, 3, 4, and 6) — predict the next bit from the preceding one to six bytes, the classic finite-context models of [[Prediction by partial matching|PPM]].
* '''Word model''' — hashes the current run of alphanumeric characters, so text is predicted from the word being spelled rather than raw byte history.
* '''Record model''' — two contexts that activate when a dominant repeating row length ''R'' is detected: the byte one record above the current position, and a combination of column index with the two bytes above. These capture the two-dimensional regularity of spreadsheets, raster scan lines, and fixed-size database records.

The remaining two models are an '''order-0 model''' (256 counters indexed directly by the
partial byte) and a '''match model'''.<ref name="format" />

=== Match model ===

The match model hashes the last six bytes into a table of 2<sup>22</sup> positions to find the
most recent identical context, then predicts that history simply repeats from that point,
with confidence learned per match-length bucket. This gives SQUISH the long-range repeat power
of [[LZ77 and LZ78|LZ77]] but with an ''unbounded'' window covering the whole file, and
probabilistically rather than as explicit tokens: a broken match costs a fraction of a bit
rather than producing a malformed match token. A bit that contradicts the predicted byte
silences the model until the next byte boundary.<ref name="squish-md" /><ref name="format" />

=== Record-length detection ===

The record model's row length ''R'' is detected automatically. A table records the last
position of each byte value; recurring distances between equal bytes are tallied as votes, and
every 2048 bytes the winning distance is examined. A vote total above 600 sets ''R'' to that
distance, while a total below 300 clears it, with a [[hysteresis]] band between to avoid
thrashing. This automatic detection is credited with SQUISH outperforming even RAR's
specialized filters on the spreadsheet file <code>kennedy.xls</code>.<ref name="squish-md" /><ref name="format" />

=== Logistic mixing ===

The models' probabilities are combined by a [[logistic regression|logistic]] mixer — a
single-layer [[neural network]] with 11 inputs — operating in the [[logit|log-odds]]
("stretch") domain. It computes a weighted sum of the stretched model outputs and squashes
the result back into a probability. One of 1024 weight vectors is selected per bit by context
(the previous byte together with the match and record state), and the active vector is trained
online by [[gradient descent]] on the actual coding loss. Models that are useless for the
current data have their weights driven toward zero within a few kilobytes; on text the word
model earns a large weight, while on binary records the record model does.<ref name="squish-md" /><ref name="format" />

=== APM/SSE refinement ===

Two chained ''adaptive probability map'' stages (also known as [[Secondary symbol estimation|secondary symbol estimation]], SSE)
refine the mixed probability. Each maps a (context, probability) pair to a corrected
probability via interpolation over a small table that is itself updated toward observed
outcomes, correcting systematic miscalibration in the mixer's output. The first stage is keyed
on the partial byte, the second on the previous byte and partial byte.<ref name="format" />

=== Arithmetic coder ===

The refined 12-bit probability feeds a carryless binary [[arithmetic coding|arithmetic coder]]
over 32-bit registers, coding bits most-significant-first within each byte. The coder never
allows the coding interval to collapse. The decoder mirrors the encoder exactly, preloading
four bytes and pulling one byte per renormalization step.<ref name="format" />

== File format ==

SQUISH has a single on-disk format, the '''SQUISH archive''', identified by the [[magic number (programming)|magic]]
bytes <code>SQSH</code> followed by <code>0x0D 0x0A 0x1A 0x0A</code>. A lone file or memory
buffer is stored as a one-member archive (marked with a <code>SINGLE</code> flag); a directory
is stored as a many-member archive.<ref name="format" />

An archive consists of a fixed 64-byte [[header (computing)|header]], a member-data region,
and a compressed index. The header records the format version, flags, member and byte counts,
the block ([[chunk (information)|chunk]]) size, and the location of the index. The index is a
single compressed block listing every member in pre-order — directories before their contents,
siblings sorted by name — so the archive contents depend only on the directory tree and not on
[[filesystem]] iteration order. Each index entry stores the member type, [[File-system permissions|Unix permission bits]],
uncompressed size, the offset of its first block, its path, and the compressed length of each
of its blocks.<ref name="format" />

The atom of the format is the '''coded block''', which holds up to one chunk of original data
as a one-byte mode field, a payload, and a 32-bit [[Fowler–Noll–Vo hash function|FNV-1a]]
[[checksum]] of the block's original bytes. Two modes exist: a context-mixed arithmetic stream,
or a ''stored'' block containing the original bytes verbatim. Encoders emit a stored block
whenever the arithmetic stream would be no smaller, which bounds the output of any block at its
input size plus five bytes and guarantees the compressor never expands incompressible data
beyond a fixed bound.<ref name="format" />

Because the index records each member's block layout, a reader can [[seek time|seek]] straight
to any member — or any block within a member — without inflating the rest of the archive,
enabling listing and single-file extraction. Extraction rejects absolute paths and paths
containing <code>..</code> components, so an archive can never write outside its target
directory (a defense against the [[Zip Slip]] class of [[directory traversal attack]]s). Each
member is limited to just under 4 [[gibibyte]]s.<ref name="format" /><ref name="readme" />

=== Parallelism ===

The model pipeline is strictly sequential within a block, since each bit's prediction depends
on the update from the previous bit. [[Multi-core processor|Multi-core]] operation therefore
comes from cutting a member into fixed-size blocks whose models start independently: separate
blocks compress and decompress in [[parallel computing|parallel]]. This yields near-linear
[[speedup]] at a cost of roughly 1–2% of compression ratio, because each block's model begins
"cold" with no accumulated history. The default single-threaded mode packs each member as one
whole-file block, which is [[Pareto efficiency|ratio-optimal]].<ref name="squish-md" /><ref name="readme" />

== Results ==

SQUISH's published benchmark combines the [[Silesia corpus]], the [[Canterbury corpus]], and
[[Hutter Prize|enwik8]] — 24 files totaling about 315&nbsp;MB — with every file verified to
round-trip exactly. Each rival compressor is run at its strongest documented setting. The
totals below are for the ratio-optimal single-block mode of SQUISH.<ref name="results">bench/RESULTS.md — benchmark results.</ref>

{| class="wikitable" style="text-align:right"
|+ Total compressed size across the 24-file, 314,749,364-byte corpus
! Compressor !! Total compressed !! Ratio
|-
| zip -9    || 104,800,190 || 0.333
|-
| bzip2 -9  ||  84,058,237 || 0.267
|-
| rar -m5   ||  80,560,956 || 0.256
|-
| xz -9e    ||  73,780,732 || 0.234
|-
| '''SQUISH''' || '''67,432,463''' || '''0.214'''
|}

Overall SQUISH compresses the corpus about 8.6% smaller than the best rival on the total, beats
zip, bzip2, and rar on 23 of 24 individual files, and beats all four (including xz -9e) on 19 of
24.<ref name="results" />

Its largest per-file margins over the best rival occur on record-structured and text data —
about 28.5% smaller than the next best on the <code>kennedy.xls</code> spreadsheet, 21.3% on the
<code>webster</code> dictionary, and 17.8% on <code>dickens</code>. The files where a rival wins
are dominated by xz: <code>nci</code> (−13.4%), <code>ptt5</code> (−16.0%), and the small
<code>sum</code> archive (−20.5%), among a handful of others.<ref name="results" />

=== Speed and memory trade-off ===

The ratio comes at a cost. Throughput is roughly 0.5–0.7&nbsp;MB/s, and because the decoder runs
the same models the process is symmetric — the full single-block corpus takes on the order of
400 seconds to compress and a similar time to decompress on the reference machine. Each active
block requires about 150&nbsp;MB of model state. The project describes this plainly as spending
CPU that zip, bzip2, and rar do not, and buying compression ratio with it. Running SQUISH in its
multi-threaded, multi-block mode recovers most of the speed — about 1.86× faster overall on the
benchmark, and up to roughly 4× on the large <code>enwik8</code> file — while giving up about
1.4% of ratio to the cold-start penalty of independent blocks.<ref name="readme" /><ref name="results" />

== Implementation ==

SQUISH is implemented in portable C with no dependencies beyond the C standard library and math
library. It builds a shared and static library, a command-line tool, and — via a [[MinGW]]
cross-compiler or [[Microsoft Visual C++|MSVC]] — a [[Dynamic-link library|Windows DLL]] and
executable. The library exposes buffer- and file-level compression functions and a
<code>squish_archive_*</code> API for packing, listing, and selectively extracting directory
trees; the shared object can be called from [[Python (programming language)|Python]] via
[[ctypes]] without a dedicated wrapper.<ref name="readme" />

The command-line tool uses subcommands to compress (<code>c</code>), decompress or restore
(<code>d</code>), list an archive (<code>l</code>), and extract a single member or subtree
(<code>x</code>), with options for thread count and block size and a live progress display.<ref name="readme" />

The implementation guarantees round-trip fidelity through per-block checksums that make
decompression fail loudly on corruption, bounded expansion through the stored-block fallback,
and thread safety through the absence of global mutable state, so independent (de)compressions
on separate buffers may run concurrently.<ref name="readme" />

== History ==

Version 1.0.0 was the initial release, comprising the context-mixing library, the CLI, the ten
models, the integrity checksum and stored-mode fallback, a test suite, the benchmark suite, and
project documentation. It was licensed under the [[GNU General Public License|GPLv3]].<ref name="changelog">CHANGELOG.md — project changelog.</ref>

Subsequent development consolidated four never-released pre-release formats — two single-stream
formats (<code>SQ01</code>, <code>SQ02</code>) and two directory-archive formats
(<code>SQAR01</code>, <code>SQAR02</code>) — into the single seekable SQUISH archive format, and
folded parallelism into the core library functions via thread-count and chunk-size parameters
rather than separate entry points. A [[self-extracting archive]] format and its
<code>squish s</code> command were removed in favor of distributing the archive and the CLI
separately. Because none of the pre-release formats had shipped, no on-disk data required
migration.<ref name="changelog" />

Per the project's contribution policy, any change to the model constants in <code>squish.c</code>
is treated as a compressed-format break requiring a new magic number, since those constants
define the bitstream.<ref name="changelog" /><ref name="format" />

== Lineage ==

SQUISH composes well-known compression primitives. Context mixing is the architecture behind the
[[PAQ]] family of compressors developed by Matt Mahoney and others, which holds many
compression-ratio records. According to its authors, SQUISH's specific set of models, its
automatically detected record contexts, its counter and mixer parameterization, and its
implementation are original to the project.<ref name="squish-md" />

== See also ==

* [[Context mixing]]
* [[PAQ]]
* [[Arithmetic coding]]
* [[Prediction by partial matching]]
* [[Lossless compression]]
* [[Data compression ratio]]
* [[Silesia corpus]]
* [[Canterbury corpus]]

== References ==

{{reflist}}

== External links ==

* [https://github.com/paigejulianne/squish SQUISH source repository]

[[Category:Free data compression software]]
[[Category:Lossless compression algorithms]]
[[Category:Archive formats]]
[[Category:C (programming language) software]]
[[Category:Cross-platform software]]
