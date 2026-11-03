// Joins soft line breaks so every paragraph, list item, and table cell is written on one line.
// Hard breaks (trailing backslash or two spaces) are `break` nodes and stay.
// A GitHub alert marker (`> [!NOTE]`) keeps its own line and is written unescaped, since GitHub only
// recognizes it alone on the first line of a blockquote.
import { visit } from 'unist-util-visit';

const softBreak = /[ \t]*\r?\n[ \t]*/g;
const alertMarker = /^\[!(?:NOTE|TIP|IMPORTANT|WARNING|CAUTION)\][ \t]*\r?\n[ \t]*/;

export default function remarkUnwrap() {
  const data = this.data();
  (data.toMarkdownExtensions ??= []).push({
    handlers: {
      text(node, parent, state, info) {
        const marker = node.data?.alertMarker;
        if (!marker) return state.safe(node.value, info);
        return `${marker}\n${state.safe(node.value.slice(marker.length + 1), info)}`;
      },
    },
  });

  return (tree) => {
    visit(tree, (node, index, parent) => {
      if (node.type !== 'text' && node.type !== 'inlineCode') return;
      const alert =
        node.type === 'text' && index === 0 && parent.type === 'paragraph' && node.value.match(alertMarker);
      if (alert) {
        const marker = alert[0].trim();
        node.value = `${marker}\n${node.value.slice(alert[0].length).replace(softBreak, ' ')}`;
        node.data = { ...node.data, alertMarker: marker };
      } else {
        node.value = node.value.replace(softBreak, ' ');
      }
    });
  };
}
