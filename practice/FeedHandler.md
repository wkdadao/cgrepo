# Exchange Feed Handler — Order Book Exercise

> Source: `FeedHandler.txt`. Reformatted as Markdown without changing the problem requirements.

## 1. Background

An exchange feed for a given financial instrument (stock, future, option, etc.) is a sequence of messages describing two distinct objects: **orders** and **trades**.

Orders are offers by market participants to buy (sell) up to a given quantity at or below (above) a specified price. Generally there are a number of orders to buy at low prices, and a number of orders to sell at high prices, since everybody is trying to get a good deal. If at any point the exchange has a buy order and a sell order at the same price, the two orders are **matched** with each other, producing a trade for the smaller of the two order quantities.

If there are multiple orders that could be matched against each other, the orders are matched first to the best possible price (i.e., the lowest-price sell or the highest-price buy order). In the event of a tie, orders are matched in **time priority** (first order to arrive will be filled first).

### 1.1 Initial order book

Orders at each price are listed from **oldest to newest** (`B` = buy, `S` = sell):

```text
Price   Orders (oldest → newest)
1100
1075    S 1             // one sell order of quantity 1
1050    S 10            // one sell order of quantity 10
1025    S 2   S 5       // older sell of quantity 2, newer sell of quantity 5

1000    B 9   B 1       // older buy of quantity 9, newer buy of quantity 1
975     B 30            // one buy order of quantity 30
950
```

The best buy order is at **1000**, and the best sell order is at **1025**. Since no seller is willing to sell low enough to match a buyer, and no buyer is willing to buy high enough to match a seller, there is no match between any of the existing orders.

### 1.2 New buy order and matching

Suppose a new buy order arrives with **quantity 3 at price 1050**:

1. The eligible standing sells are `S 2`, `S 5` (both at 1025), and `S 10` (at 1050).
2. The orders at **1025** have better price priority than the sell at 1050.
3. The older `S 2` is filled first, producing a trade of **2 @ 1025**.
4. The remaining buy quantity of 1 matches the newer `S 5`, producing a trade of **1 @ 1025**.
5. The `S 2` is removed, and the `S 5` is reduced to `S 4`.

Two trade messages are generated, together with order-related messages for removing `S 2` and modifying `S 5`. The resulting standing book is:

```text
Price   Orders (oldest → newest)
1100
1075    S 1
1050    S 10
1025    S 4

1000    B 9   B 1
975     B 30
950
```

A new sell order at 1025 would be placed **behind** the existing `S 4` in the queue. Buy and sell orders may arrive at **any price level**; the examples show only a few levels.

## 2. Message formats

### 2.1 Order message

```text
action,orderid,side,quantity,price
A,123,B,9,1000
```

| Field | Description |
|---|---|
| `action` | `A` = add, `X` = remove, `M` = modify |
| `orderid` | Unique positive integer used to reference an order for remove/modify |
| `side` | `B` = buy, `S` = sell |
| `quantity` | Positive integer representing maximum quantity to buy/sell |
| `price` | `double`: maximum buy price / minimum sell price |

### 2.2 Trade message

```text
action,quantity,price
T,2,1025
```

| Field | Description |
|---|---|
| `action` | `T` = trade |
| `quantity` | Quantity traded |
| `price` | Price at which the trade happened |

## 3. Example input

### 3.1 Build the initial book

Order `100004` is canceled and therefore does not appear in the initial book illustrated above.

```text
A,100000,S,1,1075
A,100001,B,9,1000
A,100002,B,30,975
A,100003,S,10,1050
A,100004,B,10,950
A,100005,S,2,1025
A,100006,B,1,1000
X,100004,B,10,950   // cancel
A,100007,S,5,1025
```

### 3.2 New order, trades, and book updates

```text
A,100008,B,3,1050
T,2,1025
T,1,1025
X,100008,B,3,1050
X,100005,S,2,1025
M,100007,S,4,1025   // modify remaining sell quantity
```

The final book is the one shown in §1.2.

## 4. Problem requirements

### 4.1 Construct the order book

Given a sequence of messages as defined above, construct an **in-memory representation of the current order book state**. Generate your own test dataset.

### 4.2 Human-readable book output

Print a human-readable representation of the book **every 10th message**.

### 4.3 Midquote output on every message

Output a machine-readable sequence of **midquotes** after **every message**.

\[
\text{midquote} = \frac{\text{best buy price} + \text{best sell price}}{2}
\]

The expected sequence for the **nine messages in §3.1** is:

```text
NAN      // no buys yet; midquote is undefined
1037.5   // (1075 + 1000) / 2
1037.5   // buy at 975 is below best buy
1025     // (1050 + 1000) / 2
1025     // buy at 950 is below best buy
1012.5   // (1025 + 1000) / 2
1012.5   // additional buy at 1000 changes quantity, not price
1012.5   // cancel buy at 950 does not change best price
1012.5   // additional sell at 1025 changes quantity, not price
```

### 4.4 Running traded quantity at the most recent trade price

On **every trade message**, output the total quantity traded at the **most recent trade price**. Reset the running quantity when the trade price changes—even if a previously seen trade price reappears later.

```text
Message     Output
T,2,1025  => 2@1025
T,1,1025  => 3@1025
T,1,1000  => 1@1000   // reset on price change
T,1,1025  => 1@1025   // does not restore the previous 1025 total
```

### 4.5 Robustness and error accounting

Code should be **clean, easy to understand, efficient, and robust**. It must not crash when processing:

- **a.** Corrupted messages.
- **b.** Duplicate order IDs (duplicate adds).
- **c.** Trades with no corresponding order.
- **d.** Removes with no corresponding order.
- **e.** A best sell price at or below the best buy price, but no trades occur.
- **f.** Negative, missing, or out-of-bounds prices, quantities, or order IDs.

Count how often each case occurs, and print a **summary after all messages have been processed**.

### 4.6 C++ starter program

The exercise permits use of the STL and modification of the starter program.

**Source fidelity note:** the original text contains three incomplete `#include` directives, a stray `*/`, and an unqualified `ios::in`. The snippet below preserves the visible starter program structure, removes the stray comment terminator for presentation, and marks missing headers explicitly; it is **not** a complete compilable implementation.

```cpp
// Original source contains three #include directives with no header names.
#include /* missing header 1 */
#include /* missing header 2 */
#include /* missing header 3 */

class FeedHandler {
public:
    FeedHandler();

    void processMessage(const std::string& line);
    void printCurrentOrderBook(std::ostream& os) const;
};

int main(int argc, char** argv) {
    FeedHandler feed;
    std::string line;
    const std::string filename(argv[1]);
    std::ifstream infile(filename.c_str(), ios::in);
    int counter = 0;

    while (std::getline(infile, line)) {
        feed.processMessage(line);
        if (++counter % 10 == 0) {
            feed.printCurrentOrderBook(std::cerr);
        }
    }

    feed.printCurrentOrderBook(std::cout);
    return 0;
}
```

---

*End of reformatted exercise. No solution or additional implementation assumptions are included.*
