const process = require("process");
const Bun = require("bun");

// Mirrors Jorge Luis Borges' "La biblioteca de Babel" as NARRATIVE FLAVOR: the path always
// starts with Hexagono, Pared, Estante, Libro, Pagina, Renglon, in that order, with Borges' own
// counts at each - but these don't encode anything (the firmware skips them entirely when
// reconstructing the message), so ANY number at each of those levels leads to the exact same
// file.txt. A random one is picked each time purely so it feels like wandering into a genuine,
// differently-numbered corner of the library. The actual message is encoded entirely in the
// folders AFTER that decorative intro, which are all UNIFORM: plain zero-padded numbered folders
// ("01" to CARACTER_COUNT), repeating as many times as needed. Two arrow-glyph naming schemes (8,
// then 16 single/double-direction arrows) both corrupted content on real hardware; this plain
// 20-radix numeric scheme is the only one confirmed to work reliably - do not drop CARACTER_COUNT
// below 20 without a real multi-level hardware test first. MUST stay in lockstep with
// docs/index.html and the firmware's babel_content_alphabet (src/monolith/usb_mtp_impl.c.h).
const DECORATIVE_LEVELS = [
  { name: "Hexagono", count: 47 },
  { name: "Pared",    count: 4 },
  { name: "Estante",  count: 5 },
  { name: "Libro",    count: 32 },
  { name: "Pagina",   count: 410 },
  { name: "Renglon",  count: 40 }
];
const CARACTER_COUNT = 20; // how many zero-padded-numbered folders per real (post-decorative) level

// Separate, much smaller alphabet for the file CONTENT itself - lowercase-only letters (no
// uppercase at all - closer to Borges' own story, and it also shortens the arrow-folder path
// needed per character, since a smaller alphabet means a smaller radix to encode against),
// digits, space, minimal punctuation (just "." and ",", same reasoning), and the accented
// Spanish/French letters needed to write real words in either language. Keeps paths meaningfully
// shorter per character than encoding raw UTF-8 bytes would, at the cost of only supporting plain
// text (characters outside this set are simply dropped). MUST stay in lockstep with
// docs/index.html and the firmware's babel_content_alphabet (src/monolith/usb_mtp_impl.c.h).
const CONTENT_ALPHABET = [" ",
  "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
  ".", ",",
  "á", "à", "â", "ä", "ã", "é", "è", "ê", "ë", "í", "ì", "î", "ï", "ó", "ò", "ô", "ö", "õ", "ú", "ù", "û", "ü", "ñ", "ç"];
const CONTENT_INDEX = {};
CONTENT_ALPHABET.forEach((ch, i) => { CONTENT_INDEX[ch] = i; });

// Uppercase is folded to lowercase (not dropped), matching docs/index.html's live-sanitizing
// textarea, so no letters of the message are lost.
function sanitizeContentText(text) {
  let out = "";
  for (const rawCh of text) {
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
const caracterWidth = digitWidth(CARACTER_COUNT);
while (index) {
  index--;
  const handle = index % caracterRadix;
  path = path + "\\" + padNumber(Number(handle) + 1, caracterWidth); // "/" for Mac
  index /= caracterRadix;
}

console.log(path);
