const process = require("process");
const Bun = require("bun");

// Mirrors Jorge Luis Borges' "La biblioteca de Babel" as NARRATIVE FLAVOR: the path always
// starts with Hexagono, Pared, Estante, Libro, Pagina, Renglon, in that order, with Borges' own
// counts at each - but these don't encode anything (the firmware skips them entirely when
// reconstructing the message), so ANY number at each of those levels leads to the exact same
// file.txt. A random one is picked each time purely so it feels like wandering into a genuine,
// differently-numbered corner of the library. The actual message is encoded entirely in the
// folders AFTER that decorative intro, which are all UNIFORM: a leading space plus a direction or
// six hexagon-neighbor arrows (e.g. " ↑"), no number at all, repeating as many times as needed. History: an
// unpadded 8-radix scheme using a BARE single-character name (plain digit or a lone arrow, no
// space/number) corrupted content when navigating generated paths on real hardware; any 2+
// character name fixed it reliably - the leading space here is what keeps " ↑" from collapsing
// back to the single bare arrow character known to corrupt. 6 options: the six directions out of a
// hexagon toward its neighbors, clockwise from the top (measured fastest of 4/6/12). MUST stay in lockstep
// with docs/index.html and the firmware's babel_arrow_glyphs (src/monolith/usb_mtp_impl.c.h).
const DECORATIVE_LEVELS = [
  { name: "Hexagono", count: 6 },
  { name: "Pared",    count: 4 },
  { name: "Estante",  count: 5 },
  { name: "Libro",    count: 32 },
  { name: "Pagina",   count: 410 },
  { name: "Renglon",  count: 40 }
];
// Order here MUST exactly match babel_arrow_glyphs in src/monolith/usb_mtp_impl.c.h.
const ARROW_GLYPHS = ["↑", "↗", "↘", "↓", "↙", "↖"];
const CARACTER_COUNT = ARROW_GLYPHS.length; // how many folders per real (post-decorative) level

// Separate, much smaller alphabet for the file CONTENT itself - lowercase-only letters (no
// uppercase), space, and minimal punctuation (just "." and ","). No digits and no accented
// letters, except "ñ" - closer to Borges' own story (the library's language uses a fixed, minimal set of
// "orthographic symbols": 22 letters, space, comma, period), and it shortens the folder-path
// needed per character, since a smaller alphabet means fewer bits to encode per character.
// Characters outside this set are simply dropped. MUST stay in lockstep with docs/index.html and
// the firmware's babel_content_alphabet (src/monolith/usb_mtp_impl.c.h).
const CONTENT_ALPHABET = [" ",
  "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "ñ", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
  ".", ","];
const CONTENT_INDEX = {};
CONTENT_ALPHABET.forEach((ch, i) => { CONTENT_INDEX[ch] = i; });

// Uppercase is folded to lowercase (not dropped), matching docs/index.html's live-sanitizing
// textarea, so no letters of the message are lost.
function sanitizeContentText(text) {
  let out = "";
  for (const rawCh of text.normalize("NFC")) { // NFC: a decomposed "n" + combining tilde becomes "ñ"
    const ch = rawCh.toLowerCase();
    if (CONTENT_INDEX.hasOwnProperty(ch)) out += ch;
  }
  return out;
}

// Zero-pads to the width `maxValue` needs (e.g. maxValue=410 -> width 3, so "69" becomes "069").
// Makes Explorer's default lexicographic sort order match numeric order exactly - without it, a
// human scanning a long alphabetically-sorted list of plain numbers can easily click an
// adjacent-but-wrong one. MUST match the firmware's babel_digit_width/babel_write_prefixed_number.
function digitWidth(maxValue) {
  return String(maxValue).length;
}
function padNumber(n, width) {
  let s = String(n);
  while (s.length < width) s = "0" + s;
  return s;
}

const filePath = process.argv[2];
if (!filePath) {
  console.error("No file specified!");
  process.exit(1);
}

const file = Bun.file(filePath);
if (!(await file.exists())) {
  console.error("File not found!");
  process.exit(1);
}

const text = await file.text();
const sanitized = sanitizeContentText(text);
const contentRadix = BigInt(CONTENT_ALPHABET.length);

let index = 0n;
for (let i = 0n; i < BigInt(sanitized.length); i++) {
  const charIndex = BigInt(CONTENT_INDEX[sanitized[Number(i)]]);
  index += (charIndex + 1n) * (contentRadix ** i);
}

// Deterministic (NOT random) pick for a decorative level: purely cosmetic since the firmware
// ignores these entirely, but deterministic means the SAME message always generates the exact
// SAME full path, including the decorative intro. Deliberately simple: just the character code
// of one character of the message (cycling through it per level), modulo the level's count.
function decorativePick(sanitizedText, levelIndex, count) {
  if (sanitizedText.length === 0) return 1;
  const charCode = sanitizedText.charCodeAt(levelIndex % sanitizedText.length);
  return (charCode % count) + 1;
}

// Path is built root-to-leaf (appended, not prepended): the decorative intro first, then one
// uniform-radix digit per subsequent folder - matching exactly how the firmware tells
// decorative-vs-real levels apart purely by "how many folders deep am I".
let path = "";
DECORATIVE_LEVELS.forEach((level, d) => {
  const pick = decorativePick(sanitized, d, level.count);
  path += "\\" + level.name + "-" + padNumber(pick, digitWidth(level.count)); // "/" for Mac
});

const caracterRadix = BigInt(CARACTER_COUNT);
while (index) {
  index--;
  const handle = index % caracterRadix;
  path = path + "\\" + " " + ARROW_GLYPHS[Number(handle)]; // "/" for Mac
  index /= caracterRadix;
}

console.log(path);
