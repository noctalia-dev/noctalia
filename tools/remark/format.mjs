// Formats Markdown and MDX files in place, or with --check reports files that are not formatted.
// Every result is re-parsed and compared with the input tree; a formatter change that would alter the
// document (anything beyond whitespace) aborts instead of being written.
import { readFile, writeFile } from 'node:fs/promises';
import { createProcessor } from './settings.mjs';
import remarkUnwrap from './unwrap.mjs';

const args = process.argv.slice(2);
const check = args[0] === '--check';
const files = check ? args.slice(1) : args;

function normalized(processor, source) {
  return JSON.stringify(processor.parse(source), function (key, value) {
    if (key === 'position' || key === 'data') return undefined;
    if (key === 'value' && (this.type === 'text' || this.type === 'inlineCode')) {
      return value.replace(/\s+/g, ' ');
    }
    return value;
  });
}

const processors = new Map();
let failed = 0;

for (const file of files) {
  const mdx = file.endsWith('.mdx');
  if (!processors.has(mdx)) {
    processors.set(mdx, { parser: createProcessor(mdx), formatter: createProcessor(mdx).use(remarkUnwrap) });
  }
  const { parser, formatter } = processors.get(mdx);

  const source = await readFile(file, 'utf8');
  let output;
  try {
    output = String(await formatter.process({ path: file, value: source }));
  } catch (error) {
    console.error(`${file}: ${error.reason ?? error.message}`);
    failed++;
    continue;
  }
  if (output === source) continue;

  if (normalized(parser, output) !== normalized(parser, source)) {
    console.error(`${file}: formatting would change the document structure; fix the source by hand`);
    failed++;
    continue;
  }
  if (check) {
    console.error(`${file}: not formatted`);
    failed++;
  } else {
    await writeFile(file, output);
  }
}

if (failed > 0) {
  if (check) console.error('run `just format-docs` to format');
  process.exit(1);
}
