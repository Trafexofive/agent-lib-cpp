from models import get_orders

def generate_report():
    orders = get_orders()
    total = len(orders)
    pending = sum(1 for o in orders if o["status"] == "pending")
    completed = sum(1 for o in orders if o["status"] == "completed")
    lines = [
        "=== ORDER REPORT ===",
        f"Total orders: {total}",
        f"Pending: {pending}",
        f"Completed: {completed}",
    ]
    for o in orders:
        lines.append(f"  #{o['id']}: user={o['user_id']} product={o['product']} qty={o['quantity']} status={o['status']}")
    return "\n".join(lines)

def save_report(path="/tmp/report.txt"):
    report = generate_report()
    with open(path, "w") as f:
        f.write(report)
    return path
