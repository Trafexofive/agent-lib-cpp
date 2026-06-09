from db import get_db

def get_orders(user_id=None):
    db = get_db()
    if user_id:
        return [dict(r) for r in db.execute("SELECT * FROM orders WHERE user_id = ?", (user_id,)).fetchall()]
    return [dict(r) for r in db.execute("SELECT * FROM orders").fetchall()]

def create_order(user_id, product, quantity):
    db = get_db()
    db.execute("INSERT INTO orders (user_id, product, quantity, status) VALUES (?, ?, ?, 'pending')",
               (user_id, product, quantity))
    db.commit()
    return db.execute("SELECT last_insert_rowid()").fetchone()[0]

def update_order_status(order_id, status):
    db = get_db()
    db.execute("UPDATE orders SET status = ? WHERE id = ?", (status, order_id))
    db.commit()
    return db.execute("SELECT * FROM orders WHERE id = ?", (order_id,)).fetchone() is not None

def delete_order(order_id):
    db = get_db()
    db.execute("DELETE FROM orders WHERE id = ?", (order_id,))
    db.commit()
    return db.execute("SELECT changes()").fetchone()[0] > 0

def get_user(user_id):
    db = get_db()
    user = db.execute("SELECT * FROM users WHERE id = ?", (user_id,)).fetchone()
    return dict(user) if user else None
