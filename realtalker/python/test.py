# Conver RealTalker ASCII phonemes to Human Readable


import votphon

x="ITHj3"  # TTS for "color"

for c in x:
  print ord(c)-48, votphon.VOTRAX[ord(c)-48]
