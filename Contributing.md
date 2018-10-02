# Contributing to Boomerang

Please read this document to learn how to contribute to Boomerang as effectively
as possible.


## Reporting bugs

To report a bug, ensure you have a GitHub account. Before submiting a new bug report,
please search the issue tracker to see whether the bug has been reported already.
If not, open a new issue and fill out the bug report form. Please make sure
to include a link to the binary for which Boomerang exhibits the bug (if possible),
and the version of Boomerang used to decompile the binary.


## Contributing code

 1. Make sure you have a GitHub account and fork the repository into your own account.
 2. Create a new branch from the branch you want to contribute to (usually `develop`).
 3. Commit your changes to the new branch and push the changes to your fork.
 4. Submit a pull request to the branch you want to contribute to (usually `develop`).
 5. Wait for other users to test and review your changes.


## Code style

When contributing code, please adhere to the coding style used throughout the code base.
Since the code style is now enfoced by clang-format, the best course of action is to
follow the style of the existing code and run `git clang-format` on the source
before committing to make sure the code adheres to the coding style.
When in doubt, please ask when submitting your pull request.


## Commit message style

 - A commit message should consist of a summary (at most 72 characters), optionally
   followed by a double newline and a detailed explanation.
 - Please write your commit message in the present tense (e.g. "Fix bug" instead
   of "Fixed bug". This matches with the style of the existing messages, and also
   auto-generated messages by `git merge` etc.
 - For bug fixes, please include the issue number and the bug headline in the summary.
   Note that each commit message should be understandable on its own, without consulting
   the issue tracker. Example: `Fix #0: Crash when decompiling self-recursive functions`
 - Most importantly, the commit message should describe *what* was changed and *why*.
   A commit message like "[adkfjslkdfjsdklfj](https://xkcd.com/1296/)" is not acceptable.
 - When in doubt, please ask when submitting your pull request.

