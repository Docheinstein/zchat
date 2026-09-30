# ENABLE word list, five-letter words

`words5.txt` holds the five-letter words of ENABLE (Enhanced North American Benchmark LEexicon, `enable1.txt`), one a
line, sorted: the guesses `/game wordle` accepts. ENABLE is in the public domain.

Made with: `grep -E '^[a-z]{5}$' enable1.txt | sort -u > words5.txt`, from
https://raw.githubusercontent.com/dolph/dictionary/master/enable1.txt.
