# Text-copy correction

The initial inference/ text copies were written through Windows newline
translation after decoding CRLF, creating CRCRLF. Python universal-newline
reading then interpreted those as extra blank lines, so the first local text
comparison failed. The raw files retained in scratch match E14 after removing
CR characters. inference-native/ and residency-native/ are the authoritative
UTF-8 text copies written without newline translation; original UTF-16 summary
files are decoded and PCI/interface identities redacted as elsewhere.
Original first copies remain unchanged. validation.json uses the corrected
copies. No GPU workload was rerun because of this collection error.
