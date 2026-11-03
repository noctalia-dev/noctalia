// Parses `## Heading {#id}` exactly like the docs site's remark-custom-heading-id, and writes it back unchanged.
import { micromarkHeadingId } from 'micromark-heading-id';
import { mdastHeadingId } from 'mdast-heading-id';

export default function remarkHeadingIdRoundTrip() {
  const data = this.data();
  (data.micromarkExtensions ??= []).push(micromarkHeadingId());
  (data.fromMarkdownExtensions ??= []).push(mdastHeadingId());
  (data.toMarkdownExtensions ??= []).push({
    handlers: {
      idString(node) {
        return `{#${node.value}}`;
      },
    },
  });
}
