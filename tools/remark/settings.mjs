// Parser plugins match the docs site (../noctalia-docs/astro.config.mjs): MDX, GFM, directives (Starlight asides),
// custom heading ids, and frontmatter. Math (`$...$`, `$$...$$`) is parsed so GitHub-rendered formulas are kept
// verbatim. Stringify options follow the existing docs style.
import { remark } from 'remark';
import remarkDirective from 'remark-directive';
import remarkFrontmatter from 'remark-frontmatter';
import remarkGfm from 'remark-gfm';
import remarkMdx from 'remark-mdx';
import remarkMath from 'remark-math';
import remarkHeadingId from './heading-id.mjs';

const settings = {
  bullet: '-',
  emphasis: '*',
  strong: '*',
  fence: '`',
  fences: true,
  rule: '-',
  listItemIndent: 'one',
  incrementListMarker: true,
};

export function createProcessor(mdx) {
  const processor = remark().data('settings', settings);
  if (mdx) processor.use(remarkMdx);
  return processor.use(remarkFrontmatter).use(remarkGfm).use(remarkMath).use(remarkDirective).use(remarkHeadingId);
}
