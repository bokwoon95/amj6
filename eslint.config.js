import jsdoc from "eslint-plugin-jsdoc";

export default [
  {
    files: ["src/javascript/**/*.js"],
    plugins: {
      jsdoc,
    },
    rules: {
      curly: ["error", "all"],
      "jsdoc/multiline-blocks": [
        "error",
        {
          noSingleLineBlocks: true,
          singleLineTags: [],
        },
      ],
    },
  },
];
