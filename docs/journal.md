# Reference

**X** itself are a reliable compiled language .. insert more text lately.

## Friday -> August 14, 2026:

Today in midnight i started the ast printing, due to that printing function
I was able to find a bug in ast.new.string->data.value. Which our instead of
storaging the literal value of, are storing whole write function value

expected output:

-- data:
-- node: string
-- len: 13
-- value: Hello, world!

actual output:

-- data:
-- node: string
-- len: 13
-- value: ("Hello, world!")
