// Joins soft line breaks so every paragraph, list item, and table cell is written on one line.
// Hard breaks (trailing backslash or two spaces) are `break` nodes and stay.
import { visit } from 'unist-util-visit';

const softBreak = /[ \t]*\r?\n[ \t]*/g;

export default function remarkUnwrap() {
  return (tree) => {
    visit(tree, (node) => {
      if (node.type === 'text' || node.type === 'inlineCode') {
        node.value = node.value.replace(softBreak, ' ');
      }
    });
  };
}
