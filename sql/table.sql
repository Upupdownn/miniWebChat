create database if not exists miniWebChat
    default character set utf8mb4
    collate utf8mb4_general_ci;

use miniWebChat;

CREATE TABLE users (
    id INT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(32) NOT NULL UNIQUE,
    password VARCHAR(64) NOT NULL
);

INSERT INTO users (username, password) VALUES
('user1', '123456'),
('user2', '123456'),
('user3', '123456'),
('admin', '13047104099'),
('test', 'test123');