
# Janet

##Syntax and the Parser
Janet 程序开始时是一个文本文件，只是一个字节序列，就像系统上的任何其他文件一样。 

Janet 源文件应该是 UTF-8 或 ASCII 编码。

在 Janet 编译或运行程序之前，它必须将源代码转换为数据结构。

像 Lisp 一样，Janet 源代码是同形的——代码被表示为 Janet 的核心数据结构,

因此该语言中用于操作元组、字符串和表的所有工具也可以很容易地用于操作源代码。

但是，在 Janet 代码转换为数据结构之前，它必须由 Janet 解析器读取或解析。

解析器，在 Lisp 中通常称为阅读器，是一台接收纯文本并输出可供编译器和宏使用的数据结构的机器。

在 Janet 中，它是解析器而不是读取器，因为在读取时没有代码执行。

这种方法更安全、更直接，并使语法突出显示、格式化和其他语法分析更简单。

虽然解析器不可扩展，但在 Janet 中，哲学是通过宏而不是读取器宏来扩展语言。

### nil, true and false
值 nil、true 和 false 都是可以在解析器中输入的文字。

### Symbols
Janet 符号表示为不以数字或冒号开头的字母数字字符序列。 它们还可以包含字符 !、@、$、%、^、&、*、-、_、+、=、:、<、>、.、? 以及任何不在 ASCII 范围内的 Unicode 代码点。

按照惯例，大多数符号应该全部小写，并使用破折号连接单词（有时称为 kebab case）。

来自另一个模块的符号通常包含一个斜杠，用于将模块名称与模块中定义的名称分开。
```
symbol
kebab-case-symbol
snake_case_symbol
my-module/my-function
*****
!%$^*__--__._+++===~-crazy-symbol
*global-var*
你好
```

### Keywords
Janet 关键字类似于以字符 : 开头的符号。 但是，它们的使用方式不同，编译器将它们视为常量而不是某物的名称。 
关键字主要用于表和结构中的键，或宏中的语法片段。
```
:keyword
:range
:0x0x0x0
:a-keyword
::
:
```

### Numbers
Janet 数由 IEEE 754 浮点数表示。 语法也类似于许多其他语言的语法。 
数字可以以 10 为基数书写，下划线用于将数字分成几组。 小数点可用于浮点数。
通过在数字前面加上所需的基数和字符 r，也可以用其他基数书写数字。 例如，16 可以写成 16、1_6、16r10、4r100 或 0x10。
 0x 前缀可用于十六进制，因为它很常见。 基数必须以 10 为基数书写，并且可以是 2 到 36 之间的任何整数。
 对于 10 以上的任何基数，使用字母作为数字（不区分大小写）。
 ```
 0
12
-65912
4.98
1.3e18
1.3E18
18r123C
11raaa&a
1_000_000
0xbeef
 ```
 
### Strings (不可变)
Janet 中的字符串用双引号括起来。 字符串是 8 位干净的，这意味着它们可以包含任意字节序列，包括嵌入的 0。 
要将双引号插入字符串本身，请使用反斜杠对双引号进行转义。 对于不可打印的字符，您可以使用几种常见转义之一，
使用 \xHH 转义以十六进制转义单个字节。 支持的转义是：
- \xHH 以十六进制转义单个任意字节。
- \n 换行符 (ASCII 10)
- \t 制表符 (ASCII 9)
- \r 回车（ASCII 13）
- \0 空（ASCII 0）
- \z 空（ASCII 0）
- \f 换页 (ASCII 12)
- \e 转义 (ASCII 27)
- \" 双引号 (ASCII 34)
- \uxxxx 使用 4 个十六进制数字转义 UTF-8 代码点。
- \Uxxxxxx 使用 6 个十六进制数字转义 UTF-8 代码点。
- \\ 反斜杠 (ASCII 92)
字符串还可以包含将被忽略的文字换行符。 这让我们可以定义一个不包含换行符的多行字符串。

### Long strings 
在 Janet 中表示字符串的另一种方法是长字符串或反引号分隔的字符串。 
字符串也可以定义为以一定数量的反引号开始，并以相同数量的反引号结束。
长字符串不包含转义序列； 将逐字解析所有字节，直到找到结束分隔符。 
这对于定义具有文字换行符、不可打印字符或需要许多转义序列的字符串的多行字符串非常有用。
```
"This is a string."
"This\nis\na\nstring."
"This
is
a
string."
``
This
is
a
string
``
`
This is
a string.
`
```

### Buffers ( 可变)
缓冲区类似于字符串，只是它们是可变数据结构。 Janet 中的字符串在创建后不能改变，而缓冲区可以在创建后更改。 
缓冲区的语法与字符串或长字符串的语法相同，但缓冲区必须以@ 字符为前缀。
```
@""
@"Buffer."
@``Another buffer``
@`
Yet another buffer
```

### Tuples （不可变）
元组是由括号或方括号包围的空格分隔值序列。 解析器将任何字符 ASCII 32、\0、\f、\n、\r 或 \t 视为空格。
```
(do 1 2 3)
[do 1 2 3]
```
方括号表示元组将用作元组文字而不是函数调用、宏调用或特殊形式。
如果元组有方括号，解析器将在元组上设置一个标志，让编译器知道将元组编译为构造函数。
程序员可以通过 tuple/type 函数检查元组是否有括号。

### Arrays (可变)
数组与元组相同，但有一个前导@ 来表示可变性。
```
@(:one :two :three)
@[:one :two :three]
```

### Structs (不可变)
结构由一系列用大括号包围的空格分隔的键值对表示。 序列定义为 key1、value1、key2、value2 等。
大括号之间必须有偶数个项目，否则解析器将发出解析错误信号。 任何值都可以是键或值。
但是，使用 nil 或 NaN 作为键会从解析的结构中删除该对。
```
{}
{:key1 "value1" :key2 :value2 :key3 3}
{(1 2 3) (4 5 6)}
{@[] @[]}
```

### Tables (可变)
表具有与结构相同的语法，但它们具有@ 前缀以指示它们是可变的。
```
@{}
@{:key1 "value1" :key2 :value2 :key3 3}
@{(1 2 3) (4 5 6)}
@{@[] @[]}
@{1 2 3 4 5 6}
```

### Comments
注释以 # 字符开始，一直持续到行尾。 没有多行注释。

### Shorthand
在其他编程语言中通常称为读取器宏，Janet 为某些形式提供了几种速记符号。 在 Janet 中，此语法称为前缀形式，它们不可扩展。
'x
Shorthand for (quote x)

;x
Shorthand for (splice x)  拼接

~x
Shorthand for (quasiquote x)

,x
Shorthand for (unquote x)

|(body $)
Shorthand for (short-fn (body $))

These shorthand notations can be combined in any order, allowing forms like ''x ((quote (quote x))), or ,;x ((unquote (splice x))).

### Syntax Highlighting
对于语法高亮，janet.vim 中有一些初步的 Vim 语法高亮。 然而，通用的 lisp 语法高亮应该提供良好的结果。 
还可以使用来自 Janet 源代码的 make 语法为其他程序生成 janet.tmLanguage 文件。

### Grammar
对于寻找更简洁的语法描述的任何人，用于识别 Janet 源代码的 PEG 语法如下。 PEG 语法本身类似于 EBNF。 更多关于 PEG 语法的信息可以在 PEG 部分找到。
- PEG: Parsing Expression Grammars
```
(def grammar
  ~{:ws (set " \t\r\f\n\0\v")
    :readermac (set "';~,|")
    :symchars (+ (range "09" "AZ" "az" "\x80\xFF") (set "!$%&*+-./:<?=>@^_"))
    :token (some :symchars)
    :hex (range "09" "af" "AF")
    :escape (* "\\" (+ (set "ntrzfev0\"\\")
                       (* "x" :hex :hex)
                       (* "u" [4 :hex])
                       (* "U" [6 :hex])
                       (error (constant "bad escape"))))
    :comment (* "#" (any (if-not (+ "\n" -1) 1)))
    :symbol :token
    :keyword (* ":" (any :symchars))
    :constant (* (+ "true" "false" "nil") (not :symchars))
    :bytes (* "\"" (any (+ :escape (if-not "\"" 1))) "\"")
    :string :bytes
    :buffer (* "@" :bytes)
    :long-bytes {:delim (some "`")
                 :open (capture :delim :n)
                 :close (cmt (* (not (> -1 "`")) (-> :n) ':delim) ,=)
                 :main (drop (* :open (any (if-not :close 1)) :close))}
    :long-string :long-bytes
    :long-buffer (* "@" :long-bytes)
    :number (cmt (<- :token) ,scan-number)
    :raw-value (+ :comment :constant :number :keyword
                  :string :buffer :long-string :long-buffer
                  :parray :barray :ptuple :btuple :struct :dict :symbol)
    :value (* (any (+ :ws :readermac)) :raw-value (any :ws))
    :root (any :value)
    :root2 (any (* :value :value))
    :ptuple (* "(" :root (+ ")" (error "")))
    :btuple (* "[" :root (+ "]" (error "")))
    :struct (* "{" :root2 (+ "}" (error "")))
    :parray (* "@" :ptuple)
    :barray (* "@" :btuple)
    :dict (* "@" :struct)
    :main :root})
```