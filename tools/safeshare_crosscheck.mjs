// Checks that Eflow encrypts exactly like safeshare.co, using the website's own code (src/lib/crypto.ts, src/lib/password.ts):
//
//   node tools/safeshare_crosscheck.mjs <safeshare-repo> <eflow-tests.exe>
//
// 1. Eflow seals files (empty, small, and 30 MB = two chunks); the website's decryptChunk opens them, split the way its
//    download does (FIXED_ENCRYPTED_CHUNK_SIZE).
// 2. The website's encryptChunk seals the same files; Eflow opens them.
// 3. Room passwords (ASCII and non-ASCII) hash to the same value on both sides.
// Exits non-zero on any mismatch. Needs Node 22.18+ (it loads the TypeScript files directly).
import { execFileSync } from "node:child_process";
import { randomBytes, randomUUID } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [repo, tool] = process.argv.slice(2);
if (!repo || !tool) {
    console.error("usage: node tools/safeshare_crosscheck.mjs <safeshare-repo> <eflow-tests.exe>");
    process.exit(2);
}
globalThis.window = globalThis;                         // the website code reaches WebCrypto through window.crypto
const web = await import(pathToFileURL(join(repo, "src", "lib", "crypto.ts")));
const { hashPassword } = await import(pathToFileURL(join(repo, "src", "lib", "password.ts")));

const work = mkdtempSync(join(tmpdir(), "safeshare-crosscheck-"));
let failures = 0;
const check = (ok, what) => {
    console.log(`[${ok ? "PASS" : "FAIL"}] ${what}`);
    if (!ok) failures++;
};
const eflow = (...args) => execFileSync(tool, args, { encoding: "utf8" }).trim();

try {
    const key = await web.generateKey();
    const keyText = await web.exportKeyToString(key);
    const files = { empty: Buffer.alloc(0), small: randomBytes(100), large: randomBytes(30 * 1024 * 1024) };

    for (const [name, plain] of Object.entries(files)) {
        // Eflow seals, the website opens
        const plainPath = join(work, `${name}.bin`), sealedPath = join(work, `${name}.eflow`), backPath = join(work, `${name}.back`);
        writeFileSync(plainPath, plain);
        eflow("--share-seal", keyText, plainPath, sealedPath);
        const sealed = readFileSync(sealedPath);
        const parts = [];
        for (let at = 0; at < sealed.length; at += web.FIXED_ENCRYPTED_CHUNK_SIZE)
            parts.push(Buffer.from(await web.decryptChunk(sealed.subarray(at, at + web.FIXED_ENCRYPTED_CHUNK_SIZE), key)));
        check(Buffer.concat(parts).equals(plain), `${name}: sealed by Eflow, opened by the website (${parts.length} chunk${parts.length > 1 ? "s" : ""})`);

        // the website seals, Eflow opens
        const chunks = [];
        for (let at = 0; at < plain.length || (at === 0 && chunks.length === 0); at += web.FIXED_PLAINTEXT_CHUNK_SIZE)
            chunks.push(Buffer.from(await web.encryptChunk(plain.subarray(at, at + web.FIXED_PLAINTEXT_CHUNK_SIZE), key)));
        const webPath = join(work, `${name}.web`);
        writeFileSync(webPath, Buffer.concat(chunks));
        eflow("--share-open", keyText, webPath, backPath);
        check(readFileSync(backPath).equals(plain), `${name}: sealed by the website, opened by Eflow`);
    }

    for (const password of ["k7Qm-3xTa-9pLr", "correct horse battery staple", "পাসওয়ার্ড-১২৩ 🔑", randomUUID()])
        check(eflow("--share-hash", password) === await hashPassword(password), `password hash: "${password}"`);
} finally {
    rmSync(work, { recursive: true, force: true });
}
console.log(failures ? `\n${failures} failed` : "\nall passed");
process.exit(failures ? 1 : 0);
