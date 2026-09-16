CREATE TABLE order_review(
  order_no TEXT PRIMARY KEY REFERENCES charging_order(order_no),
  user_id BIGINT NOT NULL REFERENCES user_account(id),
  rating INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 5),
  content TEXT NOT NULL CHECK(length(content) BETWEEN 1 AND 500),
  created_at BIGINT NOT NULL CHECK(created_at > 0)
);
CREATE INDEX ix_order_review_user ON order_review(user_id, created_at);
